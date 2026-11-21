"""Shared, local-only command vocabulary and complete-reply handling."""
from dataclasses import dataclass
import json
from pathlib import Path
import re
import time
from urllib.parse import urlsplit


COMMAND = re.compile(r"AT(?:\+[A-Z0-9_]+\??)?\Z")
WORD = re.compile(r"[A-Z0-9_]+\Z")
OPTION = re.compile(r"XIAOTAI_[A-Z0-9_]+\Z")
TOKEN = re.compile(r"<[^<>\[\]]+>|[\[\]]|[^\s<>\[\]]+")
END = re.compile(r"(?:OK|ERROR(?:$|[: ,].*))\Z")
DATA = re.compile(r"^\+[A-Z][A-Z0-9_-]*:")
LIMIT = 8 * 1024 * 1024
READY = "+READY: at=1 "
RESTARTED = "device restarted during the command; reconnect to verify the new session"
DENIED_PROBES = {"AT+INTENT binding.forget", "AT+INTENT provisioning.forget"}
COMPANION_IDENTITY_READS = frozenset({"AT+COMPANION", "AT+COMPANION LIVE"})
LEGACY_DIAGNOSTICS = {"version", "help", "status", "memory", "AT", "AT+VERSION", "AT+HELP", "AT+STATUS",
                      "AT+STATE", "AT+MEM", "AT+CONSOLE", "AT+CPINFO?", "AT+NET", "AT+PLATFORM", "AT+EP"}


class ConsoleError(ValueError):
    """The device or the selected policy refused the requested operation."""


def command_line(text):
    if not isinstance(text, str) or not 0 < len(text) < 768 or not all(" " <= c <= "~" for c in text):
        raise ConsoleError("AT requires one bounded printable ASCII line")
    words = text.split()
    if not 1 <= len(words) <= 12 or not COMMAND.fullmatch(words[0]):
        raise ConsoleError("expected a canonical AT command")
    return " ".join(words)


def _valid_environment_base(base):
    # Keep malformed overrides and credentials off the serial wire. The
    # firmware owns normalization and the actual storage change.
    if len(base) > 246 or any(c in base for c in "\\?#"):
        return False
    try:
        parsed = urlsplit(base)
        return bool(parsed.scheme in ("http", "https") and parsed.hostname and
                    "@" not in parsed.netloc and not parsed.netloc.endswith(":") and parsed.port != 0)
    except ValueError:
        return False


def _validate_local_arguments(words):
    # Preserve the legacy preflight for these approved forms only. Other
    # placeholders in the generated vocabulary still describe shape, not policy.
    if len(words) < 2:
        return
    command, verb, *args = words
    if command == "AT+EP" and verb == "SET" and (len(args) != 1 or not _valid_environment_base(args[0])):
        raise ConsoleError("invalid AT+EP SET base URL")
    if command != "AT+RECOGNITION":
        return
    if verb == "RECORD" and (len(args) != 1 or not re.fullmatch(r"[1-9]|[1-5][0-9]|60", args[0])):
        raise ConsoleError("AT+RECOGNITION RECORD requires 1..60 seconds")
    if verb in ("RESUME", "DISMISS", "ARCHIVE"):
        if len(args) != (1 if verb == "RESUME" else 2) or any(
            not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", value)
            for value in (args[:1] if verb == "ARCHIVE" else args)
        ):
            raise ConsoleError("invalid AT+RECOGNITION identifier")
        if verb == "ARCHIVE" and (not re.fullmatch(r"[0-9]{1,16}", args[1]) or int(args[1]) > 9007199254740991):
            raise ConsoleError("invalid AT+RECOGNITION revision")


def _forms(usage):
    """Expand the manifest's literals, alternatives and nested optional suffixes.

    Placeholders describe parameters; the firmware remains their authority.
    In particular, do not impose another product's password/range rules here.
    """
    tokens = TOKEN.findall(usage)
    if "".join(tokens) != "".join(usage.split()):
        # Whitespace inside a human-readable placeholder is legitimate.
        if "".join("".join(tokens).split()) != "".join(usage.split()):
            raise ConsoleError(f"invalid manifest usage {usage!r}")
    at = 0

    def sequence(depth=0):
        nonlocal at
        forms = [()]
        if depth > 4:
            raise ConsoleError("manifest synopsis nesting is too deep")
        while at < len(tokens):
            token = tokens[at]
            at += 1
            if token == "]":
                if depth == 0:
                    raise ConsoleError("manifest synopsis has an unmatched ']'")
                return forms
            if token == "[":
                options = [(), *sequence(depth + 1)]
            else:
                options = [(token,)]
            forms = [prefix + suffix for prefix in forms for suffix in options]
            if len(forms) > 32:
                raise ConsoleError("manifest synopsis has too many forms")
        if depth:
            raise ConsoleError("manifest synopsis has an unmatched '['")
        return forms

    return sequence()


@dataclass(frozen=True)
class Spelling:
    command: str
    verb: str | None
    usage: str
    access: str
    option: str | None

    @property
    def label(self):
        return " ".join(part for part in (self.command, self.verb, self.usage) if part)

    def matches(self, words):
        prefix = (self.command, self.verb) if self.verb else (self.command,)
        if tuple(words[:len(prefix)]) != prefix:
            return False
        args = words[len(prefix):]
        return any(len(form) == len(args) and all(
            token.startswith("<") or word in token.split("|") for token, word in zip(form, args)
        ) for form in _forms(self.usage))


class Vocabulary:
    def __init__(self, data, enabled=None, board=None):
        if not isinstance(data, dict) or not re.fullmatch(r"[a-z0-9][a-z0-9-]{0,79}", str(data.get("product", ""))):
            raise ConsoleError("expected product-generated commands.json")
        self.product = data["product"]
        self.board = board
        if not isinstance(data.get("commands"), list) or not 0 < len(data["commands"]) <= 512:
            raise ConsoleError("invalid commands.json entries")
        enabled = enabled or {}
        self.entries = []
        labels = set()
        for row in data["commands"]:
            if not isinstance(row, dict):
                raise ConsoleError("invalid command entry")
            command, verb, usage, access, option = (row.get(key) for key in ("command", "verb", "usage", "access", "option"))
            if (not isinstance(command, str) or not COMMAND.fullmatch(command)
                    or (verb is not None and (not isinstance(verb, str) or not WORD.fullmatch(verb)))
                    or not isinstance(usage, str) or len(usage) > 128 or any(not " " <= c <= "~" for c in usage)
                    or access not in ("read", "action", "guarded")
                    or (option is not None and (not isinstance(option, str) or not OPTION.fullmatch(option)))):
                raise ConsoleError("invalid command spelling or access in commands.json")
            entry = Spelling(command, verb, usage, access, option)
            _forms(usage)
            if entry.label in labels:
                raise ConsoleError("duplicate command spelling in commands.json")
            labels.add(entry.label)
            self.entries.append((entry, enabled.get(option, True)))
        self.verbs = {(entry.command, entry.verb) for entry, _ in self.entries if entry.verb}

    def authorize(self, line, permits=(), advertised=None):
        words = command_line(line).split()
        named = len(words) > 1 and tuple(words[:2]) in self.verbs
        matches = [(entry, enabled) for entry, enabled in self.entries
                   if bool(entry.verb) == named and entry.matches(words)]
        if not matches:
            raise ConsoleError("command or argument form is absent from this product's vocabulary")
        if len(matches) != 1:
            raise ConsoleError("command form is ambiguous in commands.json")
        entry, enabled = matches[0]
        if not enabled or (advertised is not None and entry.label not in advertised):
            raise ConsoleError(f"running firmware does not expose {entry.label}; check its options and AT+HELP gate")
        if entry.access != "read" and " ".join(words) not in permits and " ".join(words) not in DENIED_PROBES:
            raise ConsoleError(f"{entry.label} requires its exact local command in --allow")
        _validate_local_arguments(words)
        return entry


def _enabled(path):
    options = {}
    if path.is_file():
        for line in path.read_text(encoding="utf-8").splitlines():
            match = re.fullmatch(r"CONFIG_(XIAOTAI_[A-Z0-9_]+)=([yn])", line)
            off = re.fullmatch(r"# CONFIG_(XIAOTAI_[A-Z0-9_]+) is not set", line)
            if match:
                options[match[1]] = match[2] == "y"
            elif off:
                options[off[1]] = False
    return options


def load_vocabulary(path):
    path = Path(path)
    try:
        if path.stat().st_size > 256 * 1024:
            raise ConsoleError("commands.json is too large")
        data = json.loads(path.read_text(encoding="utf-8"))
        enabled = _enabled(path.with_name("sdkconfig.selection"))
        enabled.update(_enabled(path.with_name("sdkconfig")))
        if enabled.get("XIAOTAI_AT_RESTART_COMMAND") is False:
            enabled["XIAOTAI_AT_RESTART_FORCE"] = False
        result = Vocabulary(data, enabled)
        root = Path(__file__).resolve().parents[1]
        product_path = root / "products" / result.product / "product.json"
        from sdk_selection import SelectionError, apply_variant, load_product, plan
        if not product_path.is_file():
            raise ConsoleError("commands.json does not name a product in this checkout")
        try:
            product = load_product(product_path)
            candidates = [product, *(apply_variant(product, name) for name in product.get("variants", {}))]
            if not any(plan(root, item)["commands"] == data["commands"] for item in candidates):
                raise ConsoleError("commands.json differs from this checkout's product manifests; "
                                   "use the checkout and commands.json from the running firmware's build")
        except SelectionError as error:
            raise ConsoleError(f"invalid product selection: {error}") from error
        board_path = root / "boards" / product["board"] / "board.json"
        result.board = json.loads(board_path.read_text(encoding="utf-8"))["identity"]["model"]
        return result
    except (OSError, KeyError, TypeError, json.JSONDecodeError) as error:
        raise ConsoleError(f"could not read product vocabulary: {error}") from error


def fields(lines, prefix):
    records = [line[len(prefix):] for line in lines if line.startswith(prefix)]
    if len(records) != 1:
        raise ConsoleError(f"expected one {prefix.strip()} record")
    parsed = {}
    for word in records[0].split():
        key, split, value = word.partition("=")
        if not split or not key or not value or key in parsed:
            raise ConsoleError(f"malformed {prefix.strip()} record")
        parsed[key] = value
    return parsed


def companion_identity(lines):
    """Validate one fresh SDK identity reply and preserve the capture's metadata keys."""
    complete(lines, "companion identity")
    records = [line for line in lines[:-1] if line.startswith("+COMPANION:")]
    if len(records) != 1 or any(END.fullmatch(line) for line in lines[:-1]):
        raise ConsoleError("expected one complete companion identity record")
    prefix = "+COMPANION: "
    if not records[0].startswith(prefix):
        raise ConsoleError("malformed companion identity record")
    required = {"project", "version", "platform", "protocol", "elf_sha256"}
    known = {}
    for word in records[0][len(prefix):].split(" "):
        key, sep, value = word.partition("=")
        if not sep or not re.fullmatch(r"[A-Za-z][A-Za-z0-9_-]*", key) or not re.fullmatch(r"[!-~]+", value):
            raise ConsoleError("malformed companion identity field")
        if key not in required:
            continue
        valid = (re.fullmatch(r"[0-9a-f]{64}", value) if key == "elf_sha256"
                 else len(value) <= 32 and "," not in value and "=" not in value)
        if key in known or not valid:
            raise ConsoleError("invalid or duplicate companion identity field")
        known[key] = value
    if known.keys() != required:
        raise ConsoleError("missing companion identity field")
    return {"project": known["project"], "version": known["version"], "idf": known["platform"], "elf": known["elf_sha256"]}


_PLATFORM_STATS_KEYS = """
clock_attempts discoveries services_invalid logins login_failures token_renewals signaling_connects
signaling_drops signaling_rebuilds subscribe_retries heartbeats requests replies request_failures requests_stale
requests_refused requests_busy transfers transfers_cancelled reply_post_retries slot_clamps signals
signals_delivered signals_acknowledged signal_post_retries signals_malformed signals_oversized
signals_fragment_errors signals_no_slot signals_stale signals_unauthorized unbound_reports profile_reports
profile_failures profile_deferrals profile_parts_dropped event_post_retries worker_stack_free
""".split()


def platform_stats(lines):
    """Decode a complete AT+PLATFORM or AT+STATUS platform snapshot."""
    complete(lines, "platform diagnostics")
    if any(END.fullmatch(line) for line in lines[:-1]):
        raise ConsoleError("multiple platform diagnostics terminators")
    fields(lines, "+PLATFORM: ")
    prefix = "+PLATFORM-STATS: "
    values = {}
    for line in lines:
        if not line.startswith(prefix):
            continue
        row = fields([line], prefix)
        if not row or len(line) > 512 or values.keys() & row.keys():
            raise ConsoleError("invalid or duplicate platform statistics")
        for key, value in row.items():
            if not re.fullmatch(r"[a-z][a-z0-9_]*", key) or not re.fullmatch(r"[0-9]{1,10}", value):
                raise ConsoleError("malformed platform statistics field")
            count = int(value)
            if count > 0xffffffff:
                raise ConsoleError("platform statistic exceeds uint32")
            values[key] = count
    if list(values)[:len(_PLATFORM_STATS_KEYS)] != _PLATFORM_STATS_KEYS:
        raise ConsoleError("missing or reordered platform statistics")
    return values


def identity(lines):
    try:
        version = fields(lines, "+VERSION: ")
    except ConsoleError:
        if any(line.startswith("+VERSION: ") and " at=" in line for line in lines):
            raise
        return None
    if "at" not in version:
        return None
    if version["at"] != "1" or not version.get("board") or not version.get("chip") or lines[-1:] != ["OK"]:
        raise ConsoleError("incomplete or unknown SDK AT identity")
    sha = fields(lines, "+BUILD: ").get("elf_sha256", "")
    if not re.fullmatch(r"(?:[0-9a-fA-F]{64}|-)", sha):
        raise ConsoleError("invalid SDK ELF fingerprint")
    return {**version, "elf_sha256": sha.lower()}


def sdk_evidence(lines):
    return any(line.startswith("+READY: at=") or
               (line.startswith("+VERSION: ") and " at=" in line) or
               line in {"ERROR: INVALID_STATE CLI_DISABLED", "ERROR: INVALID_STATE STARTING"}
               for line in lines)


def legacy_preflight(lines, *, read_only=False, confirmed=False):
    if sdk_evidence(lines):
        raise ConsoleError("SDK AT firmware detected or locked; use generated --commands and exact local permissions")
    if read_only or confirmed:
        return
    for line in lines:
        if re.fullmatch(r"\+VERSION: board=\S+ (?:revision=\S+ )?version=\S+ idf=\S+ mac=[0-9A-Fa-f:]{12,17}.*", line):
            return
        if re.fullmatch(r"Firmware: version=\S+ built=.+ elf-sha256=\S+", line):
            return
    raise ConsoleError("unidentified console: only read-only diagnostics are allowed without confirmed legacy firmware")


class Session:
    def __init__(self, exchange, vocabulary, permits=(), expected_sha=None, detected=None):
        self.exchange = exchange
        self.vocab = vocabulary
        self.permits = frozenset(command_line(p) for p in permits)
        self.expected_sha = expected_sha.lower() if expected_sha else None
        self.usable = True
        if self.expected_sha and not re.fullmatch(r"[0-9a-f]{64}", self.expected_sha):
            raise ConsoleError("expected a full 64-digit ELF SHA-256")
        self.device = detected or identity(complete(self.exchange("AT+VERSION", 3)))
        if not self.device:
            raise ConsoleError("this firmware did not identify itself as SDK AT at=1")
        if self.vocab.board and self.device["board"] != self.vocab.board:
            raise ConsoleError("running board does not match the product vocabulary")
        if self.expected_sha and self.expected_sha != self.device["elf_sha256"]:
            raise ConsoleError("running ELF fingerprint does not match the confirmed build")
        self.advertised = help_records(complete(self.exchange("AT+HELP", 3)))

    def has(self, command):
        command = command_line(command)
        try:
            self.vocab.authorize(command, {command}, self.advertised)
        except ConsoleError:
            return False
        return True

    def check(self, command):
        if not self.usable:
            raise ConsoleError("previous serial reply was incomplete; reopen and identify before another command")
        command = command_line(command)
        entry = self.vocab.authorize(command, self.permits, self.advertised)
        if entry.access != "read":
            if not self.vocab.board or not self.expected_sha:
                raise ConsoleError("local action requires selected board identity and --elf-sha256 of the build")
        return entry

    def send(self, command, wait=3):
        command = command_line(command)
        entry = self.check(command)
        try:
            answer = self.exchange(command, wait)
            if not answer or not END.fullmatch(answer[-1]):
                raise ConsoleError("incomplete serial command reply")
            if any(line.startswith(READY) for line in answer):
                raise ConsoleError(RESTARTED)
        except ConsoleError:
            self.usable = False
            raise
        if command in DENIED_PROBES:
            if answer[-1:] != ["ERROR: NOT_ALLOWED"]:
                raise ConsoleError("a local-only intent probe did not answer ERROR: NOT_ALLOWED")
        elif command in ("AT+PLATFORM", "AT+STATUS platform"):
            platform_stats(answer)
        elif command in COMPANION_IDENTITY_READS:
            companion_identity(answer)
        else:
            complete(answer, entry.label)
        return answer


def complete(lines, description="command"):
    if not lines or not END.fullmatch(lines[-1]):
        raise ConsoleError(f"{description}: timed out before the final reply")
    if lines[-1] != "OK":
        # Never include a raw command/password or an untrusted payload in an error.
        tail = re.fullmatch(r"ERROR(?:: ([A-Z0-9_]+(?: [A-Z0-9_]+)?))?", lines[-1])
        reason = tail[1] if tail and tail[1] else "ERROR"
        raise ConsoleError(f"{description}: device answered {reason}")
    return lines


def help_records(lines):
    prefix = "+HELP: "
    records = [line[len(prefix):] for line in lines if line.startswith(prefix)]
    if not records or len(records) != len(set(records)) or "AT" not in records:
        raise ConsoleError("AT+HELP returned no usable or duplicate records")
    return frozenset(records)


class ReplyReader:
    """Incremental line framing, no overall stream timeout on forward progress."""

    def __init__(self, wait, clock=time.monotonic):
        self.wait = wait
        self.clock = clock
        self.deadline = clock() + wait
        self.partial = bytearray()
        self.seen = 0
        self.lines = []
        self.done = False

    def receive(self, data):
        self.seen += len(data)
        if self.seen > LIMIT:
            raise ConsoleError("serial reply exceeded the bounded capture size")
        self.partial.extend(data)
        *lines, partial = self.partial.split(b"\n")
        self.partial = partial
        for raw in lines:
            line = raw.removesuffix(b"\r").decode("utf-8", "replace")
            # A boot marker is not command progress, even if a later boot
            # emits a plausible terminator in this same chunk.
            if line.startswith(READY):
                raise ConsoleError(RESTARTED)
            if self.done:
                if END.fullmatch(line):
                    raise ConsoleError("multiple serial command terminators")
                continue
            self.lines.append(line)
            # Only data records, not background firmware logs, keep a stream alive.
            if DATA.match(line):
                self.deadline = self.clock() + self.wait
            self.done = bool(END.fullmatch(line))
        if len(self.partial) > 4096:
            raise ConsoleError("unterminated serial line exceeds the capture limit")


def exchange_uart(port, command, wait, capture=lambda _: None, clock=None, allow_incomplete=False):
    clock = clock or time.monotonic
    # A finished previous reply or old logs are not evidence for the next write.
    for _ in range(64):
        if not port.in_waiting:
            break
        capture(port.read(min(port.in_waiting, 65536)))
    else:
        raise ConsoleError("could not establish a fresh reply boundary before writing")
    reader = ReplyReader(wait, clock)
    port.write((command_line(command) + "\r\n").encode("ascii"))
    port.flush()
    while not reader.done and clock() < reader.deadline:
        data = port.read(max(1, min(port.in_waiting, 65536)))
        if data:
            capture(data)
            reader.receive(data)
    if not reader.done and not allow_incomplete:
        raise ConsoleError("timed out before the final serial reply; a later command was not sent")
    return reader.lines


def hex_text(text):
    return text.encode("utf-8").hex().upper() if text else "-"
