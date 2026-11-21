"""Environment commands cannot change a device during ordinary log capture."""
import json
import unittest

import test_device_console_recognition as recognition_fixture


class EnvironmentConsoleTests(unittest.TestCase):
    invoke = recognition_fixture.RecognitionConsoleTests.invoke

    def test_show_is_read_only(self):
        code, output = self.invoke(['AT+EP', 'AT+EP SHOW', 'ep-show'])
        self.assertEqual(code, 0)
        self.assertTrue(json.loads(output)['dry_run'])

    def test_changes_require_their_own_flag(self):
        for command in ('AT+EP RESET', 'ep-reset', 'AT+EP CANCEL', 'ep-cancel',
                        'AT+EP SET http://fixture.test:8080/pre', 'ep-set https://fixture.test/pre'):
            self.assertEqual(self.invoke([command])[0], 2)
            self.assertEqual(self.invoke([command], ['--allow-ui-actions', '--allow-recognition-actions',
                                                   '--allow-recognition-upload'])[0], 2)
            self.assertEqual(self.invoke([command], ['--allow-environment-changes'])[0], 0)

    def test_invalid_input_never_reaches_serial(self):
        bad = ('http://', 'mqtt://host', 'http://user:secret@host', 'http://host?token=secret',
               'http://host#part', 'http://host:0', 'http://host:65536', 'http://host:port',
               'http://[::1', 'http://host\\path', 'http://host path', 'http://host\nAT',
               'http://host/' + 'a' * 247)
        for base in bad:
            self.assertEqual(self.invoke(['ep-set ' + base], ['--allow-environment-changes'])[0], 2, base)
        for command in ('ep-reset extra', 'AT+EP SET', 'AT+EP=DEFAULT', 'AT+EP?'):
            self.assertEqual(self.invoke([command], ['--allow-environment-changes'])[0], 2, command)

    def test_flag_does_not_authorize_other_operations(self):
        for command in ('AT+RECOGNITION RECORD 1', 'tirtc-clear', 'wifi-clear', 'restart'):
            self.assertEqual(self.invoke([command], ['--allow-environment-changes'])[0], 2)


if __name__ == '__main__':
    unittest.main()
