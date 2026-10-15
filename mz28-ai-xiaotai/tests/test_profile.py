import json
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ProfileContractTests(unittest.TestCase):
    def test_minimal_audio_profile_matches_device_server_contract(self):
        source = (ROOT / "components/wechat_voip/voip.c").read_text(encoding="utf-8")
        declaration = re.search(r"static const char s_profile\[\]\s*=\s*(.*?);", source, re.S)
        self.assertIsNotNone(declaration)
        literals = re.findall(r'"(?:\\.|[^"\\])*"', declaration.group(1))
        profile = json.loads("".join(json.loads(value) for value in literals))
        # The server rejects unknown fields. up_audio_mt belongs to stream/call, not voip.
        self.assertEqual(profile, {"profiles": {"voip": {
            "audio_rate": 16000,
            "audio_channels": 1,
            "down_audio_mt": "opus",
            "up_video_mt": "none",
            "down_video_mt": "none",
            "no_video": True,
            "calling_timeout_sec": 30,
        }}})
