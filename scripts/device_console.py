"""Bounded local console capture; resets only when explicitly requested."""
import argparse
import datetime
import json
from pathlib import Path
import time

import serial

parser = argparse.ArgumentParser(description='Trusted local serial only. Raw logs/screens may contain personal data; store outside Git.')
parser.add_argument('--port', required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--seconds', type=float, default=15)
parser.add_argument('--command', action='append', default=[])
parser.add_argument('--command-delay', type=float, default=1)
parser.add_argument('--command-interval', type=float, default=3)
parser.add_argument('--reset-uart', action='store_true')
parser.add_argument('--reset-usb', action='store_true')
parser.add_argument('--idf-open', action='store_true')
args = parser.parse_args()
if not 0 < args.seconds <= 600:
    parser.error('Capture duration must be in (0, 600] seconds')
if args.command_delay < 0 or args.command_interval < 1:
    parser.error('Command delay must be nonnegative; interval must be at least one second')
allowed = {'version', 'status', 'help', 'ai-start', 'ai-stop', 'voice-test self-test',
           'AT', 'AT+HELP', 'AT+VERSION', 'AT+STATUS', 'AT+UI', 'AT+SCREEN', 'AT+AI START', 'AT+AI STOP'}
if args.reset_uart and args.reset_usb:
    parser.error('Select at most one reset transport')
if args.command and args.command_delay + (len(args.command) - 1) * args.command_interval >= args.seconds:
    parser.error('Capture must extend beyond the last scheduled command')
if any(command not in allowed for command in args.command):
    parser.error('Only reviewed local diagnostic/session commands are supported')
args.output.parent.mkdir(parents=True, exist_ok=True)
port = serial.Serial(port=None, baudrate=115200, timeout=0.2, write_timeout=2)
port.dtr = args.idf_open
port.rts = args.idf_open
port.port = args.port
started = time.monotonic()
sent = []
total = 0
try:
    with args.output.open('xb') as raw:
        port.open()
        if args.idf_open:
            port.rts = False
            port.dtr = False
        if args.reset_uart or args.reset_usb:
            from esptool.reset import HardReset
            HardReset(port, uses_usb=args.reset_usb)()
        while time.monotonic() - started < args.seconds:
            elapsed = time.monotonic() - started
            if len(sent) < len(args.command) and elapsed >= args.command_delay + len(sent) * args.command_interval:
                command = args.command[len(sent)]
                port.write((command + '\r\n').encode('utf-8'))
                port.flush()
                sent.append({'seconds': round(elapsed, 3), 'command': command})
            data = port.read(max(1, min(port.in_waiting, 65536)))
            if data:
                raw.write(data)
                raw.flush()
                total += len(data)
finally:
    port.close()
    metadata = {'port': args.port, 'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
                'seconds': round(time.monotonic() - started, 3), 'bytes': total,
                'commands': sent, 'reset_requested': args.reset_uart or args.reset_usb,
                'idf_open_sequence': args.idf_open}
    with args.output.with_suffix('.json').open('x', encoding='utf-8') as output:
        json.dump(metadata, output, indent=2)
    print(json.dumps(metadata))
