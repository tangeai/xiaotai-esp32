"""Bounded local console capture; resets only when explicitly requested."""
import argparse
import datetime
import json
from pathlib import Path
import re
import sys
import time
from urllib.parse import urlsplit

CPINFO = 'AT+CPINFO?'
CPINFO_LINE = re.compile(
    rb'\+CPINFO:project=([^,=]{1,32}),version=([^,=]{1,32}),idf=([^,=]{1,32}),elf=([0-9a-f]{64})'
)
LEGACY_READS = {'version', 'status', 'help', 'AT', 'AT+HELP', 'AT+VERSION', 'AT+STATUS', 'AT+UI',
                'AT+UI PREFS', 'AT+SCREEN', 'AT+AI STATE', 'AT+RECOGNITION CONFIG', 'AT+RECOGNITION STATE',
                'AT+EP', 'AT+EP SHOW', 'ep-show', CPINFO}


def read_companion_info(log, offset):
    if offset is None:
        raise ValueError('Companion query was not sent')
    identity = None
    with log.open('rb') as source:
        source.seek(offset)
        for raw in source:
            if not raw.endswith(b'\n'):
                break
            line = raw.rstrip(b'\r\n')
            if line.startswith(b'+CPINFO:'):
                match = CPINFO_LINE.fullmatch(line)
                if identity is not None or not match or any(
                        not value.isascii() or not value.decode('ascii').isprintable() or not value.strip()
                        for value in match.groups()):
                    raise ValueError('Invalid or duplicate companion identity')
                identity = dict(zip(('project', 'version', 'idf', 'elf'),
                                    (value.decode('ascii') for value in match.groups())))
            elif line == b'OK':
                if identity is None:
                    raise ValueError('Companion query returned OK without identity')
                return identity
            elif line == b'ERROR' or line.startswith((b'ERROR,', b'ERROR:')):
                raise ValueError('Companion query returned ERROR; see raw capture')
    raise ValueError('No complete companion reply before capture ended')


def recognition_command_allowed(command, actions=False, upload=False):
    if upload and re.fullmatch(r'AT\+RECOGNITION RECORD (?:[1-9]|[1-5][0-9]|60)', command):
        return True
    if not actions:
        return False
    if command in {'AT+RECOGNITION QUERY', 'AT+RECOGNITION CANCEL'}:
        return True
    identifier = r'[A-Za-z0-9_-]{1,64}'
    if re.fullmatch(r'AT\+RECOGNITION RESUME ' + identifier, command):
        return True
    if re.fullmatch(r'AT\+RECOGNITION DISMISS ' + identifier + ' ' + identifier, command):
        return True
    match = re.fullmatch(r'AT\+RECOGNITION ARCHIVE ' + identifier + r' ([0-9]{1,16})', command)
    return bool(match and int(match[1]) <= 9007199254740991)


def environment_command_allowed(command, changes=False):
    if not changes:
        return False
    if command in {'AT+EP RESET', 'ep-reset', 'AT+EP CANCEL', 'ep-cancel'}:
        return True
    match = re.fullmatch(r'(?:AT\+EP SET|ep-set) (.+)', command)
    if not match:
        return False
    base = match[1]
    if len(base) >= 247 or any(ord(c) <= 32 or ord(c) >= 127 or c in '\\?#' for c in base):
        return False
    try:
        url = urlsplit(base)
        return bool(url.scheme in {'http', 'https'} and url.hostname and url.username is None and
                    url.password is None and (url.port is None or 1 <= url.port <= 65535))
    except ValueError:
        return False


parser = argparse.ArgumentParser(description='Trusted local serial only. Raw logs/screens may contain personal data; store outside Git.')
parser.add_argument('--port', required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--seconds', type=float, default=15)
parser.add_argument('--command', action='append', default=[])
parser.add_argument('--commands', type=Path, metavar='COMMANDS_JSON', help='Use the generated SDK product vocabulary')
parser.add_argument('--legacy', action='store_true', help='Confirm older firmware that cannot answer AT+VERSION')
parser.add_argument('--allow', action='append', default=[], metavar='EXACT_COMMAND',
                    help='Permit precisely this local SDK action; repeat per action')
parser.add_argument('--elf-sha256', help='Confirm the full running SDK firmware SHA-256 before an action')
parser.add_argument('--command-delay', type=float, default=1)
parser.add_argument('--command-interval', type=float, default=3)
parser.add_argument('--idle-timeout', type=float, default=5, help='SDK wait for the next reply record, not a total stream limit')
parser.add_argument('--reset-uart', action='store_true')
parser.add_argument('--reset-usb', action='store_true')
parser.add_argument('--idf-open', action='store_true')
parser.add_argument('--allow-ui-actions', action='store_true', help='Permit local diagnostic tap and display sleep')
parser.add_argument('--allow-recognition-actions', action='store_true', help='Permit task query/playback, cancel, resume, dismiss and archive; not recording')
parser.add_argument('--allow-recognition-upload', action='store_true', help='Permit microphone recording and upload that may consume service quota')
parser.add_argument('--allow-environment-changes', action='store_true', help='Permit discovery environment set/reset and cancel; changes persist and restart with existing identity')
parser.add_argument('--dry-run', action='store_true', help='Validate and print commands without opening a port or creating the capture file')
args = parser.parse_args()
if not 0 < args.seconds <= 600:
    parser.error('Capture duration must be in (0, 600] seconds')
if not 0 < args.idle_timeout <= 120:
    parser.error('Reply idle timeout must be in (0, 120] seconds')
if args.command_delay < 0 or args.command_interval < 1:
    parser.error('Command delay must be nonnegative; interval must be at least one second')
if args.commands:
    # Keep legacy script invocation (including runpy tests) independent of the SDK policy.
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from console_sdk import (COMPANION_IDENTITY_READS, ConsoleError, Session, command_line,
                             companion_identity, exchange_uart, load_vocabulary)

    if args.legacy:
        parser.error('--legacy cannot be used with --commands')
    if args.reset_uart or args.reset_usb or args.idf_open:
        parser.error('SDK command capture never changes reset lines')
    if args.allow_ui_actions or args.allow_recognition_actions or args.allow_recognition_upload or args.allow_environment_changes:
        parser.error('SDK commands use exact --allow entries, not legacy permission flags')
    try:
        batch = [command_line(line) for line in args.command]
        read_companion = any(line in COMPANION_IDENTITY_READS for line in batch)
        if read_companion and len(batch) != 1:
            parser.error('A companion identity query must be the only capture command')
        vocabulary = load_vocabulary(args.commands)
        grants = {command_line(line) for line in args.allow}
        forms = [vocabulary.authorize(line, grants) for line in batch]
        # A dry run reports spellings, never materialized credential arguments.
        if args.dry_run:
            print(json.dumps({'dry_run': True, 'product': vocabulary.product,
                              'commands': [entry.label for entry in forms]}))
            raise SystemExit(0)
        import serial

        args.output.parent.mkdir(parents=True, exist_ok=True)
        port = serial.Serial(port=None, baudrate=115200, timeout=0.05, write_timeout=2)
        port.dtr = port.rts = False
        port.port = args.port
        sent = []
        started = time.monotonic()
        error_text = None
        companion = None
        with args.output.open('xb') as raw:
            try:
                port.open()
                session = Session(lambda line, wait: exchange_uart(port, line, wait, raw.write),
                                  vocabulary, grants, args.elf_sha256)
                for line in batch:
                    session.check(line)
                for line, entry in zip(batch, forms):
                    response = session.send(line, args.idle_timeout)
                    if read_companion:
                        companion = companion_identity(response)
                    sent.append({'spelling': entry.label, 'terminal': response[-1]})
                while time.monotonic() - started < args.seconds:
                    data = port.read(max(1, min(port.in_waiting, 65536)))
                    if data:
                        raw.write(data)
            except (ConsoleError, serial.SerialException) as error:
                error_text = str(error)
            finally:
                port.close()
            count = raw.tell()
        metadata = {'product': vocabulary.product, 'port': args.port, 'bytes': count, 'commands': sent}
        if companion is not None and not error_text:
            metadata['companion'] = companion
        if error_text:
            metadata['error'] = error_text
            if read_companion:
                metadata['companion_error'] = error_text
        with args.output.with_suffix('.json').open('x', encoding='utf-8') as output:
            json.dump(metadata, output, indent=2)
        print(json.dumps(metadata))
        raise SystemExit(1 if error_text else 0)
    except (ConsoleError, OSError) as error:
        parser.error(str(error))
if args.allow or args.elf_sha256:
    parser.error('--allow and --elf-sha256 require --commands')
allowed = LEGACY_READS | {'ai-start', 'ai-stop', 'voice-test self-test', 'AT+AI START', 'AT+AI STOP',
                          'AT+CALL ACCEPT', 'AT+CALL REJECT', 'AT+CALL HANGUP'}
companion_query = CPINFO in args.command
if companion_query and (args.command != [CPINFO] or args.reset_uart or args.reset_usb or args.idf_open):
    parser.error('AT+CPINFO? must be the only command, without reset or --idf-open')
if args.reset_uart and args.reset_usb:
    parser.error('Select at most one reset transport')
if args.command and args.command_delay + (len(args.command) - 1) * args.command_interval >= args.seconds:
    parser.error('Capture must extend beyond the last scheduled command')
if any(command not in allowed and re.fullmatch(r'AT\+CALL DIAL [A-Za-z0-9_-]{1,64}', command) is None
       and not recognition_command_allowed(command, args.allow_recognition_actions, args.allow_recognition_upload)
       and not environment_command_allowed(command, args.allow_environment_changes)
       and not (args.allow_ui_actions and
                (command == 'AT+UI SLEEP' or re.fullmatch(r'AT\+TAP [0-9]{1,5} [0-9]{1,5}', command)))
       for command in args.command):
    parser.error('Only reviewed commands are supported; UI, recognition and environment changes require their explicit allow flags')
if args.dry_run:
    print(json.dumps({'dry_run': True, 'port': args.port, 'commands': args.command,
                      'reset_uart': args.reset_uart, 'reset_usb': args.reset_usb, 'idf_open': args.idf_open}))
    raise SystemExit(0)
import serial

args.output.parent.mkdir(parents=True, exist_ok=True)
port = serial.Serial(port=None, baudrate=115200, timeout=0.2, write_timeout=2)
port.dtr = args.idf_open
port.rts = args.idf_open
port.port = args.port
started = time.monotonic()
sent = []
total = 0
companion_offset = None
companion = None
companion_error = None
capture_error = None
try:
    with args.output.open('xb') as raw:
        port.open()
        if args.idf_open:
            port.rts = False
            port.dtr = False
        if args.reset_uart or args.reset_usb:
            from esptool.reset import HardReset
            HardReset(port, uses_usb=args.reset_usb)()
        if any(command not in LEGACY_READS for command in args.command):
            sys.path.insert(0, str(Path(__file__).resolve().parent))
            from console_sdk import ConsoleError, exchange_uart, legacy_preflight

            probe = bytearray()

            def capture_probe(data):
                raw.write(data)
                if len(probe) < 8192:
                    probe.extend(data[:8192 - len(probe)])

            try:
                identity = exchange_uart(port, 'AT+VERSION', 2, capture_probe, allow_incomplete=True)
                # Stale input can disqualify this target as legacy, but only
                # a fresh identity reply can qualify it without explicit assurance.
                legacy_preflight(probe.decode('utf-8', 'replace').splitlines(), read_only=True)
                legacy_preflight(identity, confirmed=args.legacy)
            except ConsoleError as error:
                capture_error = str(error)
            total = raw.tell()
            started = time.monotonic()
        while capture_error is None and time.monotonic() - started < args.seconds:
            elapsed = time.monotonic() - started
            if len(sent) < len(args.command) and elapsed >= args.command_delay + len(sent) * args.command_interval:
                command = args.command[len(sent)]
                if command != CPINFO or not port.in_waiting:
                    if command == CPINFO:
                        companion_offset = raw.tell()
                    port.write((command + '\r\n').encode('utf-8'))
                    port.flush()
                    sent.append({'seconds': round(elapsed, 3), 'command': command})
            data = port.read(max(1, min(port.in_waiting, 65536)))
            if data:
                raw.write(data)
                raw.flush()
                total += len(data)
    if companion_query:
        try:
            companion = read_companion_info(args.output, companion_offset)
        except ValueError as error:
            companion_error = str(error)
finally:
    port.close()
    metadata = {'port': args.port, 'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
                'seconds': round(time.monotonic() - started, 3), 'bytes': total,
                'commands': sent, 'reset_requested': args.reset_uart or args.reset_usb,
                'idf_open_sequence': args.idf_open}
    if companion is not None:
        metadata['companion'] = companion
    if companion_error is not None:
        metadata['companion_error'] = companion_error
    if capture_error is not None:
        metadata['error'] = capture_error
    with args.output.with_suffix('.json').open('x', encoding='utf-8') as output:
        json.dump(metadata, output, indent=2)
    print(json.dumps(metadata))
if companion_error is not None or capture_error is not None:
    raise SystemExit(1)
