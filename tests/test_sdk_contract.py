import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class CryptoContract(unittest.TestCase):
    def validate(self, actual, expected):
        with tempfile.TemporaryDirectory() as temp:
            script = Path(temp) / "test.cmake"
            code = f'include("{(ROOT / "components/tirtc_sdk/crypto_contract.cmake").as_posix()}")\n'
            code += "\n".join(f'set({key} "{value}")' for key, value in actual.items())
            code += "\ntirtc_validate_crypto_contract([==[" + json.dumps({"crypto_config": expected}) + "]==])\n"
            script.write_text(code, encoding="utf-8")
            return subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True)

    def test_matching_contract(self):
        fields = {"CONFIG_MBEDTLS_SSL_PROTO_TLS1_2": "y", "CONFIG_PSA_SOMETHING": "0"}
        self.assertEqual(self.validate(fields, fields).returncode, 0)

    def test_missing_setting_rejected(self):
        result = self.validate({}, {"CONFIG_MBEDTLS_SSL_PROTO_TLS1_2": "y"})
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ABI mismatch", result.stderr)

    def test_different_numeric_setting_rejected(self):
        self.assertNotEqual(self.validate({"CONFIG_MBEDTLS_LIMIT": "8"}, {"CONFIG_MBEDTLS_LIMIT": "16"}).returncode, 0)

    def test_added_ds_peripheral_rejected(self):
        required = {"CONFIG_MBEDTLS_SSL_PROTO_TLS1_2": "y"}
        result = self.validate({**required, "CONFIG_MBEDTLS_HARDWARE_RSA_DS_PERIPHERAL": "y"}, required)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("additional application setting", result.stderr)

    def test_disabled_extra_setting_allowed(self):
        required = {"CONFIG_MBEDTLS_SSL_PROTO_TLS1_2": "y"}
        self.assertEqual(self.validate({**required, "CONFIG_MBEDTLS_DEBUG": ""}, required).returncode, 0)

    def test_empty_contract_rejected(self):
        self.assertNotEqual(self.validate({}, {}).returncode, 0)


if __name__ == "__main__":
    unittest.main()
