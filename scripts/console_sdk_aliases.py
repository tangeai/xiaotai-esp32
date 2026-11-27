"""Old bench input translated on the SDK dialect only.

The spelling tables mirror the migration spec, xiaotai_at.md sections 4.3
and 10. They do not grant access: the caller must check every resulting
line against the build vocabulary, live HELP and local authorization.
"""
import re
import shlex

from console_sdk import ConsoleError, command_line, hex_text


class AliasError(ConsoleError):
    """A legacy spelling cannot be translated faithfully."""


# Simple one-to-one forms. The last two sections of the spec also describe
# old log-only fields; those are handled by the bench's status readers.
ALIASES = {
    "version": ("AT+VERSION",), "status": ("AT+STATUS",), "help": ("AT+HELP",),
    "restart": ("AT+RESTART",), "AT+REBOOT": ("AT+RESTART",), "memory": ("AT+MEM",),
    "AT+CLI?": ("AT+CLI",), "AT+CACHE": ("AT+STATUS port",),
    "AT+DIAG USB_TICK": ("AT+PORT PROBE usb_tick",),
    "AT+HW?": ("AT+STATUS board",), "AT+HW": ("AT+STATUS board",),
    "AT+LCD?": ("AT+STATUS display",), "AT+STATUS?": ("AT+STATUS",),
    "AT+UI PREFS": ("AT+SET", "AT+STATUS preferences"),
    "AT+GUIDANCE": ("AT+SET guidance.enabled", "AT+STATUS preferences"),
    "AT+AI STATE": ("AT+AI",), "ai-start": ("AT+AI START",), "ai-stop": ("AT+AI STOP",),
    "AT+WAKE?": ("AT+WAKE",), "voice-test self-test": ("AT+WAKE SELFTEST",),
    "AT+CALL?": ("AT+CALL",), "AT+ANSWER": ("AT+CALL ACCEPT",), "answer": ("AT+CALL ACCEPT",),
    "AT+REJECT": ("AT+CALL REJECT",), "reject": ("AT+CALL REJECT",),
    "AT+HANGUP": ("AT+CALL HANGUP",), "hangup": ("AT+CALL HANGUP",), "cancel": ("AT+CALL HANGUP",),
    "AT+CONTACTS?": ("AT+CONTACTS",), "contacts": ("AT+CONTACTS REFRESH",),
    "AT+CONTACTS=REFRESH": ("AT+CONTACTS REFRESH",),
    "AT+ROOM STATUS": ("AT+ROOM",), "AT+ROOM?": ("AT+ROOM",),
    "AT+ROOM=CREATE": ("AT+ROOM CREATE",),
    "AT+GROUP PTT_DOWN": ("AT+ROOM PTT 1",), "AT+GROUP PTT_UP": ("AT+ROOM PTT 0",),
    "AT+RECOGNITION STATE": ("AT+RECOGNITION",),
    "AT+WIFI?": ("AT+NET",), "AT+NET?": ("AT+NET",), "AT+NETWORK": ("AT+NET",),
    "wifi-clear": ("AT+NET FORGET",), "AT+WIFI SETUP": ("AT+NET PROVISION",),
    "AT+WIFI CANCEL": ("AT+NET CANCEL",), "AT+DISCONNECT": ("AT+NET DISCONNECT",),
    "AT+NET=DISCONNECT": ("AT+NET DISCONNECT",), "AT+NETDIAG?": ("AT+NET DIAG",),
    "AT+NETRX=START": ("AT+NET PROBE",), "AT+PTRACE?": ("AT+NET TRACE",),
    "AT+COMPANIONRETRY": ("AT+COMPANION RETRY",), "AT+C6RETRY": ("AT+COMPANION RETRY",),
    "AT+C61=ROLLBACK": ("AT+COMPANION ROLLBACK",),
    "AT+EP SHOW": ("AT+EP",), "ep-show": ("AT+EP",),
    "ep-reset": ("AT+EP RESET",), "ep-cancel": ("AT+EP CANCEL",),
    "AT+PRODUCT": ("AT+BIND", "AT+PLATFORM", "AT+NET"), "id": ("AT+BIND",),
    "AT+DEVICE?": ("AT+VERSION", "AT+BIND"), "retry": ("AT+BIND RETRY",),
    "tirtc-clear": ("AT+BIND CLEAR",), "AT+UI?": ("AT+UI", "AT+UI ACTIONS"),
    "AT+DISPLAY": ("AT+UI REDRAW",), "display": ("AT+UI REDRAW",), "AT+DOOR?": ("AT+DOOR",),
    "tone": ("AT+AUDIO TONE",), "record": ("AT+AUDIO RECORD",),
    "play": ("AT+AUDIO PLAY",), "stop": ("AT+AUDIO STOP",),
    "AT+MEDIA TONE": ("AT+AUDIO SENDTONE",), "AT+CMDTRACE": ("AT+LINK TRACE",),
    "AT+RTC": ("AT+LINK", "AT+CALL", "AT+ROOM"),
    "AT+CAMTEST": ("AT+VIDEO PROBE",), "AT+CAMINFO?": ("AT+STATUS board",),
    "prompt success": ("AT+SOUND PLAY cue.success",), "ring stop": ("AT+SOUND STOP",),
    "cue wifi": ("AT+SOUND PLAY guide.wifi_setup",),
}

# Historical developer probes explicitly retired in section 4.3.
RETIRED = (
    (r"AT\+ECHO(?: .*)?", "use AT+STATUS media to read echo statistics"),
    (r"rx-(?:dump|record)(?: .*)?", "the legacy MZ28 entry was disabled; use AT+STATUS media for diagnostics"),
    (r"AT\+AUDIO DETAIL(?: .*)?", "use AT+STATUS media to inspect the frontend"),
    (r"AT\+AUDIO RXTRACE(?: .*)?", "use AT+LOG TOPICS or AT+TRACE for packet/frame diagnostics"),
    (r"(?:AT\+TXTRACE(?:[=?].*)?|AT\+TRACE TX(?: .*)?)", "use AT+STATUS media and AT+LOG TOPICS"),
    (r"AT\+CAMERA(?: .*)?", "use AT+VIDEO PROBE for self-test or AT+STATUS board for sensors"),
    (r"AT\+USB(?: .*)?", "use AT+STATUS port, or a named AT+PORT PROBE if advertised"),
    (r"AT\+CP(?: .*)?", "use AT+COMPANION and the subcommands shown in AT+HELP"),
)

_GUIDE = ("startup", "wifi_setup", "wifi_connecting", "wifi_connected", "wifi_failed", "binding", "bound",
          "offline", "online", "incoming", "dialing", "ended", "failed")

# The entire input must match. String substitution never treats an argument
# as an executable, shell fragment or second serial record.
_FORMS = tuple((re.compile(pattern), target) for pattern, target in (
    (r"AT\+CLI=(ON|OFF)", r"AT+CLI \1"),
    (r"voice-config ([0-9]{1,3})", r"AT+WAKE THRESHOLD \1"),
    (r"voice-diag( [0-9]{1,2})?", r"AT+WAKE WATCH\1"),
    (r"AT\+AI=(START|STOP)", r"AT+AI \1"),
    (r"AT\+CALL DIAL ([^ ]+)", r"AT+CALL DEVICE \1"),
    (r"AT\+CALL[ =]([0-9]{1,5})", r"AT+CALL INDEX \1"),
    (r"AT\+VIDCALL=([0-9]{1,5})", r"AT+CALL VIDEO \1"),
    (r"AT\+CALL=(ACCEPT|REJECT|HANGUP)", r"AT+CALL \1"),
    (r"AT\+CALL=(CAMERA|MUTE),([01])", r"AT+CALL \1 \2"),
    (r"AT\+CONTACTS (ALL|DEV|WX)( [0-9]{1,5})?", r"AT+CONTACTS PAGE \1\2"),
    (r"AT\+PENDING( [0-9]{1,5})?", r"AT+CONTACTS PENDING\1"),
    (r"AT\+GROUP (REFRESH|LEAVE|CONNECT|DISCONNECT)", r"AT+ROOM \1"),
    (r"AT\+GROUP ((?:CREATE|JOIN)(?: [0-9]+){0,2})", r"AT+ROOM \1"),
    (r"AT\+ROOM=(OPEN|CLOSE|REFRESH|LEAVE)", r"AT+ROOM \1"),
    (r"(?:AT\+PTT |AT\+ROOM=PTT,)([01])", r"AT+ROOM PTT \1"),
    (r"AT\+ROOM=CREATE,([0-9]{4})", r"AT+ROOM CREATE \1"),
    (r"AT\+ROOM=JOIN,([0-9]{6}),([0-9]{4})", r"AT+ROOM JOIN \1 \2"),
    (r"AT\+ROOM=JOIN,([0-9]{6})", r"AT+ROOM JOIN \1"),
    (r"AT\+PTRACE=(START|STOP)", r"AT+NET TRACE \1"),
    (r"AT\+C61=(INFO|RXRESET|SINK0|SINK1)", r"AT+COMPANION SEND \1"),
    (r"AT\+CPOTA=([^ ,]+),([0-9a-fA-F]{64})", r"AT+COMPANION UPDATE \1 \2"),
    (r"ep-set ([^ ]+)", r"AT+EP SET \1"),
    (r"AT\+TAP ([0-9]+ [0-9]+)", r"AT+UI TAP \1"),
    (r"AT\+SWIPE ((?:[0-9]+ ){4}[0-9]+)", r"AT+UI SWIPE \1"),
    (r"AT\+UI=POINTER,([0-9]+),([0-9]+),([01])", r"AT+UI POINTER \1 \2 \3"),
    (r"AT\+DOOR=(HOME|NEXT|PREVIOUS|VIEWER|SENDER|DEVICES|BACK)", r"AT+DOOR \1"),
    (r"AT\+GUIDANCE ([01])", r"AT+SET guidance.enabled \1"),
    (r"ring (incoming|outgoing)", r"AT+SOUND PLAY ring.\1"),
    (r"cue (ended|warning|tap)", r"AT+SOUND PLAY cue.\1"),
    (r"prompt ([0-9]{6})", r"AT+SOUND PLAY guide.binding \1"),
))


def _translated(*lines):
    try:
        return tuple(command_line(line) for line in lines)
    except ConsoleError:
        raise AliasError("unknown legacy spelling or invalid canonical AT line; see AT+HELP") from None


def _legacy_words(text, label, count):
    try:
        words = shlex.split(text, posix=True)
    except ValueError:
        raise AliasError(f"invalid quoted arguments for {label}") from None
    if len(words) not in count:
        raise AliasError(f"invalid argument count for {label}")
    return words


def translate(line, board):
    """Return canonical SDK lines; an empty tuple denotes a stateful workflow."""
    if not isinstance(line, str) or not 0 < len(line) < 768 or not all(c.isprintable() for c in line):
        raise AliasError("expected one bounded printable console line")
    original = line.strip()
    # Quoted legacy free text/credentials retain their exact spaces.
    command = " ".join(original.split())
    model = (board or "").lower()
    xmini = model in {"xmini", "xmini-c3"}
    cores3 = model in {"cores3", "m5stack-cores3-lite"}
    wt99 = model in {"wt99", "wt99p461-s1"}

    for pattern, advice in RETIRED:
        if re.fullmatch(pattern, command):
            raise AliasError(f"legacy diagnostic is retired in SDK firmware; {advice}")
    if command in ALIASES:
        return ALIASES[command]
    if command == "AT+RETRY" or original.startswith("wifi {"):
        return ()
    if command == "AT+CPINFO?":
        return ("AT+COMPANION LIVE" if wt99 else "AT+COMPANION",)
    if command == "AT+AUDIOTRACE?":
        # Wave 6 retains the WT99 on-device alias and its idle-only output.
        return (command if wt99 else "AT+TRACE DUMP",)
    if command.startswith("voice-test "):
        phrase, = _legacy_words(original[11:], "voice-test", {1})
        return ("AT+WAKE SELFTEST",) if phrase == "self-test" else _translated("AT+WAKE TEST " + hex_text(phrase))
    if (match := re.fullmatch(r"call ([1-8])", command)):
        return (f"AT+CALL INDEX {int(match[1]) - 1}",)
    if (match := re.fullmatch(r"AT\+(VOL|MIC) ([0-9]{1,3})", command)):
        kind, value = match[1], int(match[2])
        if kind == "VOL" and (xmini or cores3) and value <= (100 if xmini else 10):
            return (f"AT+SET audio.volume {value * (10 if cores3 else 1)}",)
        if kind == "MIC" and cores3 and value <= 1:
            return (f"AT+CALL MUTE {1 - value}",)
        if kind == "MIC" and xmini and value <= 36:
            return (f"AT+SET audio.mic_gain {value}",)
        raise AliasError("legacy volume/microphone form does not match this board; use canonical AT+SET or AT+CALL MUTE")
    if (match := re.fullmatch(r"AT\+GUIDE ([0-9]{1,2})(?: ([0-9]{6}))?", command)):
        index, code = int(match[1]), match[2]
        if xmini and index == 5 and not code:
            raise AliasError("legacy Xmini binding guidance requires exactly six digits")
        if index >= len(_GUIDE) or (code and index != 5):
            raise AliasError("unknown legacy guidance sound; use AT+SOUND to list names")
        return _translated(f"AT+SOUND PLAY guide.{_GUIDE[index]} {code or ''}")
    if (match := re.fullmatch(r"video-capture (start|stop|status|clear|dump)", command)):
        return ("AT+VIDEO CAPTURE " + match[1].upper(),)
    if original.startswith("tirtc-set "):
        device, key, *client = _legacy_words(original[10:], "tirtc-set", {2, 3})
        return _translated(" ".join(("AT+BIND SET", device, hex_text(key), *client)))
    if original.startswith("AT+WIFI="):
        chunks = original[8:].split(",")
        if len(chunks) != 2 or not chunks[0]:
            raise AliasError("ambiguous legacy Wi-Fi fields; use canonical AT+NET WIFI with hex fields")
        return _translated("AT+NET WIFI " + " ".join(hex_text(chunk) for chunk in chunks))
    if original.startswith(("wifi-set ", "AT+WIFI ")):
        if xmini and original.startswith("AT+WIFI "):
            match = re.fullmatch(r"AT\+WIFI ([0-9a-fA-F]+),([0-9a-fA-F]*|-)", original)
            if not match or len(match[1]) % 2 or (match[2] != "-" and len(match[2]) % 2):
                raise AliasError("expected legacy Xmini hex credentials; use canonical AT+NET WIFI")
            return (f"AT+NET WIFI {match[1].upper()} {(match[2] or '-').upper()}",)
        tag, _, payload = original.partition(" ")
        ssid, password = _legacy_words(payload, tag, {2})
        if not ssid:
            raise AliasError("Wi-Fi SSID is required")
        return _translated(f"AT+NET WIFI {hex_text(ssid)} {hex_text(password)}")
    for pattern, target in _FORMS:
        if (match := pattern.fullmatch(command)):
            return _translated(match.expand(target))
    return _translated(command)
