# Replaces legacy: tests/test_sdk_contract.py
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


class ConfigTables(unittest.TestCase):
    def validate(self, actual, contract):
        with tempfile.TemporaryDirectory() as temp:
            script = Path(temp) / "test.cmake"
            code = f'include("{(ROOT / "components/tirtc_sdk/config_contract.cmake").as_posix()}")\n'
            code += "\n".join(f'set({key} "{value}")' for key, value in actual.items())
            code += "\ntirtc_validate_config_tables([==[" + json.dumps(contract) + "]==])\n"
            script.write_text(code, encoding="utf-8")
            return subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True)

    def test_every_table_is_checked(self):
        contract = {"libc_config": {"CONFIG_LIBC_PICOLIBC": "y"}, "memory_config": {"CONFIG_SPIRAM": "y"}}
        self.assertEqual(self.validate({"CONFIG_LIBC_PICOLIBC": "y", "CONFIG_SPIRAM": "y"}, contract).returncode, 0)
        without_psram = self.validate({"CONFIG_LIBC_PICOLIBC": "y"}, contract)
        self.assertNotEqual(without_psram.returncode, 0)
        self.assertIn("memory_config mismatch: CONFIG_SPIRAM requires y", without_psram.stderr)
        self.assertNotEqual(self.validate({"CONFIG_LIBC_NEWLIB": "y", "CONFIG_SPIRAM": "y"}, contract).returncode, 0)

    def test_absent_or_empty_tables_ask_nothing(self):
        self.assertEqual(self.validate({}, {}).returncode, 0)
        self.assertEqual(self.validate({}, {"atomic_config": {}, "memory_config": {}}).returncode, 0)


class MemoryDeclaration(unittest.TestCase):
    def setUp(self):
        import sys
        sys.path.insert(0, str(ROOT / "scripts"))
        import tirtc_contract
        self.memory_config = tirtc_contract.memory_config

    def test_declared_psram_is_required_on_any_chip(self):
        self.assertEqual(self.memory_config({"PSRAM": "enabled"}, {"malloc"}), {"CONFIG_SPIRAM": "y"})
        self.assertEqual(self.memory_config({"PSRAM": "enabled"}, {"heap_caps_malloc"}), {"CONFIG_SPIRAM": "y"})

    def test_archive_using_capability_allocators_must_declare(self):
        for call in ("heap_caps_malloc", "xTaskCreatePinnedToCoreWithCaps"):
            with self.assertRaises(SystemExit):
                self.memory_config({}, {call, "malloc"})
        self.assertEqual(self.memory_config({"PSRAM": "disabled"}, {"heap_caps_malloc"}), {})

    def test_plain_archive_needs_no_declaration(self):
        self.assertEqual(self.memory_config({}, {"malloc", "xTaskCreatePinnedToCore"}), {})

    def test_unknown_value_rejected(self):
        with self.assertRaises(SystemExit):
            self.memory_config({"PSRAM": "yes"}, set())


if __name__ == "__main__":
    unittest.main()
