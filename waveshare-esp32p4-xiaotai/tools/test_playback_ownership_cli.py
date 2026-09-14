"""Check historical-source selection without Git, a C compiler or a device."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'tools/test_playback_ownership.py'


class BaselineSelectionTest(unittest.TestCase):
    def reject(self, arguments, message):
        result = subprocess.run(
            [sys.executable, '-B', '-X', 'utf8', str(SCRIPT), *arguments],
            cwd=ROOT, capture_output=True, text=True, encoding='utf-8', timeout=10)
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn(message, result.stderr)
        self.assertNotIn('Traceback', result.stderr)
        self.assertNotIn('fatal: path', result.stderr)

    def test_requires_explicit_source(self):
        self.reject(['--baseline'], 'expected one argument')

    def test_missing_source_is_reported(self):
        with tempfile.TemporaryDirectory(prefix='p4-baseline-') as directory:
            self.reject(['--baseline', str(Path(directory) / 'missing.c')],
                        'cannot read baseline source')

    def test_current_layout_is_not_a_historical_baseline(self):
        self.reject(['--baseline', str(ROOT / 'components/starter_media/src/starter_media.c')],
                    'historical play_audio_item')


if __name__ == '__main__':
    unittest.main()
