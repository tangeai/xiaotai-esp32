"""SDK-only translations from the migration table (4.3), without serial IO."""
# Replaces legacy: tests/test_at_diagnostics.py
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from console_sdk import ConsoleError, Vocabulary, hex_text
from console_sdk_aliases import ALIASES, AliasError, translate
from sdk_selection import apply_variant, load_product, plan

S3 = "lckfb-szpi-esp32s3"
CORE = "m5stack-cores3-lite"
XMINI = "xmini-c3"
WT = "wt99p461-s1"
P4 = "waveshare-p4-touch-lcd-3.5"
MZ = "mz28-ai-ppd"

# Independent examples of the two sides of the spec table, including the
# spellings that have more than one output, or vary with the old board.
CASES = (
    (S3, "version", ("AT+VERSION",)),
    (S3, "AT+UI PREFS", ("AT+SET", "AT+STATUS preferences")),
    (S3, "AT+CALL DIAL abc_5", ("AT+CALL DEVICE abc_5",)),
    (S3, "AT+TAP 10 90", ("AT+UI TAP 10 90",)),
    (S3, "voice-config 700", ("AT+WAKE THRESHOLD 700",)),
    (S3, "voice-diag", ("AT+WAKE WATCH",)),
    (S3, "voice-diag 30", ("AT+WAKE WATCH 30",)),
    (S3, "voice-test self-test", ("AT+WAKE SELFTEST",)),
    (S3, 'voice-test "phrase with  spaces"', ("AT+WAKE TEST " + hex_text("phrase with  spaces"),)),
    (S3, 'wifi-set "a b" "c d"', ("AT+NET WIFI 612062 632064",)),
    (S3, "AT+RECOGNITION CONFIG", ("AT+RECOGNITION CONFIG",)),
    (S3, "ep-set http://example.test", ("AT+EP SET http://example.test",)),
    (CORE, "AT+CALL 2", ("AT+CALL INDEX 2",)),
    (CORE, "AT+VOL 7", ("AT+SET audio.volume 70",)),
    (CORE, "AT+MIC 0", ("AT+CALL MUTE 1",)),
    (CORE, "AT+MIC 1", ("AT+CALL MUTE 0",)),
    (CORE, "AT+PTT 1", ("AT+ROOM PTT 1",)),
    (CORE, "AT+SWIPE 0 1 2 3 200", ("AT+UI SWIPE 0 1 2 3 200",)),
    (CORE, "tirtc-set device key client", ("AT+BIND SET device 6B6579 client",)),
    (XMINI, "AT+VOL 7", ("AT+SET audio.volume 7",)),
    (XMINI, "AT+MIC 20", ("AT+SET audio.mic_gain 20",)),
    (XMINI, "AT+WIFI 6162,63", ("AT+NET WIFI 6162 63",)),
    (XMINI, "AT+WIFI 6162,-", ("AT+NET WIFI 6162 -",)),
    (XMINI, "AT+RETRY", ()),
    (XMINI, "AT+CONTACTS WX 2", ("AT+CONTACTS PAGE WX 2",)),
    (XMINI, "AT+GROUP PTT_UP", ("AT+ROOM PTT 0",)),
    (XMINI, "AT+GROUP JOIN 123456 1723", ("AT+ROOM JOIN 123456 1723",)),
    (XMINI, "AT+PRODUCT", ("AT+BIND", "AT+PLATFORM", "AT+NET")),
    (XMINI, "AT+RTC", ("AT+LINK", "AT+CALL", "AT+ROOM")),
    (XMINI, "AT+GUIDANCE 0", ("AT+SET guidance.enabled 0",)),
    (XMINI, "AT+GUIDANCE", ("AT+SET guidance.enabled", "AT+STATUS preferences")),
    (XMINI, "AT+PENDING 7", ("AT+CONTACTS PENDING 7",)),
    (XMINI, "AT+GUIDE 5 123456", ("AT+SOUND PLAY guide.binding 123456",)),
    (XMINI, "AT+RECOGNITION CONFIG", ("AT+RECOGNITION CONFIG",)),
    (WT, "AT+CALL=4", ("AT+CALL INDEX 4",)),
    (WT, "AT+VIDCALL=3", ("AT+CALL VIDEO 3",)),
    (WT, "AT+CALL=CAMERA,1", ("AT+CALL CAMERA 1",)),
    (WT, "AT+ROOM=CREATE", ("AT+ROOM CREATE",)),
    (WT, "AT+ROOM=JOIN,123456,0010", ("AT+ROOM JOIN 123456 0010",)),
    (WT, "AT+ROOM=JOIN,123456", ("AT+ROOM JOIN 123456",)),
    (WT, "AT+PTRACE=STOP", ("AT+NET TRACE STOP",)),
    (WT, "AT+CPINFO?", ("AT+COMPANION LIVE",)),
    (WT, "AT+C61=RXRESET", ("AT+COMPANION SEND RXRESET",)),
    (WT, "AT+CPOTA=http://example.test/c61.bin," + "23" * 32,
     ("AT+COMPANION UPDATE http://example.test/c61.bin " + "23" * 32,)),
    (WT, "AT+CAMTEST", ("AT+VIDEO PROBE",)),
    (WT, "AT+AUDIOTRACE?", ("AT+AUDIOTRACE?",)),
    (P4, "AT+CPINFO?", ("AT+COMPANION",)),
    (P4, "AT+WIFI=ab,", ("AT+NET WIFI 6162 -",)),
    (P4, "AT+CLI=OFF", ("AT+CLI OFF",)),
    (P4, "video-capture status", ("AT+VIDEO CAPTURE STATUS",)),
    (P4, "video-capture dump", ("AT+VIDEO CAPTURE DUMP",)),
    (MZ, "call 1", ("AT+CALL INDEX 0",)),
    (MZ, "call 8", ("AT+CALL INDEX 7",)),
    (MZ, 'wifi {"ssid":"test","password":"short"}', ()),
    (MZ, "prompt 123456", ("AT+SOUND PLAY guide.binding 123456",)),
    (MZ, "ring incoming", ("AT+SOUND PLAY ring.incoming",)),
)


class AliasTests(unittest.TestCase):
    def test_migration_examples(self):
        for board, command, expected in CASES:
            with self.subTest(board=board, command=command):
                self.assertEqual(translate(command, board), expected)

    def test_retired_forms_report_alternatives_without_a_wire_line(self):
        for command, alternate in (
            ("AT+ECHO 1", "AT+STATUS media"),
            ("rx-record", "AT+STATUS media"),
            ("AT+AUDIO DETAIL 0", "AT+STATUS media"),
            ("AT+AUDIO RXTRACE", "AT+LOG TOPICS"),
            ("AT+TXTRACE=START", "AT+STATUS media"),
            ("AT+TRACE TX STOP", "AT+STATUS media"),
            ("AT+CAMERA TEST", "AT+VIDEO PROBE"),
            ("AT+USB TICK", "AT+STATUS port"),
        ):
            with self.subTest(command=command):
                with self.assertRaisesRegex(AliasError, alternate.replace("+", r"\+")):
                    translate(command, WT)

    def test_canonical_and_malformed_lines(self):
        self.assertEqual(translate("AT+STATE", MZ), ("AT+STATE",))
        for command in ("", "AT\r\nAT+RESET", "unknown-legacy", "call 0", "AT+VOL 11",
                        "AT+WIFI=ab,cd,ef", 'tirtc-set a "unterminated', "AT+GUIDE 7 123456"):
            with self.subTest(command=command):
                with self.assertRaises(AliasError):
                    translate(command, CORE)
        with self.assertRaises(AliasError):
            translate("AT+WIFI 6162,A", XMINI)

    def test_xmini_legacy_binding_guide_requires_code(self):
        with self.assertRaisesRegex(AliasError, "exactly six digits"):
            translate("AT+GUIDE 5", XMINI)
        self.assertEqual(translate("AT+GUIDE 5 012345", XMINI), ("AT+SOUND PLAY guide.binding 012345",))
        self.assertEqual(translate("AT+GUIDE 0", XMINI), ("AT+SOUND PLAY guide.startup",))
        self.assertEqual(translate("AT+SOUND PLAY guide.binding", XMINI), ("AT+SOUND PLAY guide.binding",))

    def test_alias_targets_exist_in_product_generated_words(self):
        vocabularies = []
        for path in sorted((ROOT / "products").glob("*/product.json")):
            product = load_product(path)
            selections = [product] + [apply_variant(product, name) for name in product.get("variants", {})]
            for selection in selections:
                selected = plan(ROOT, selection)
                vocabularies.append(Vocabulary({"product": selected["product"], "commands": selected["commands"]}))

        def in_manifest(line, vocabulary):
            try:
                vocabulary.authorize(line, {line})
            except ConsoleError:
                return False
            return True

        # The target has to be declared, not necessarily built in every
        # product. Actual product options and HELP are checked again at use.
        targets = {line for values in ALIASES.values() for line in values}
        targets.update(line for _, _, values in CASES for line in values)
        for line in sorted(targets):
            with self.subTest(target=line):
                self.assertTrue(any(in_manifest(line, vocabulary) for vocabulary in vocabularies))


if __name__ == "__main__":
    unittest.main()
