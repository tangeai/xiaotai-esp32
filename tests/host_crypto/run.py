"""Execute the production signing function against the pinned IDF 6 PSA library."""
import argparse
import base64
import hashlib
import hmac
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/regression"))
from test_voice_reliability import function

parser = argparse.ArgumentParser()
parser.add_argument("--idf-path", required=True, type=Path)
args = parser.parse_args()
source = args.idf_path / "components/mbedtls/mbedtls/tf-psa-crypto"
out = ROOT / "build/host/psa"
out.mkdir(parents=True, exist_ok=True)
fixture = out / "fixture.c"
code = '''#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "psa/crypto.h"
#include "mbedtls/base64.h"
#include "mbedtls/platform_util.h"
typedef int esp_err_t;
enum {ESP_OK=0, ESP_FAIL=-1, ESP_ERR_INVALID_ARG=2, ESP_ERR_INVALID_SIZE=3, ESP_ERR_INVALID_CRC=4};
'''
code += function("main/xiaotai/services/device_platform/src/platform_client.c", "hmac_signature_with_key")
code += function("components/xiaotai_crypto/xiaotai_crypto.c", "xiaotai_hmac_sha256")
code += function("main/xiaotai/input/wt99/starter_at/src/coprocessor_update.c", "verify_image_sha256")
code += '\nint main(void) { char output[64]; uint8_t digest[32];\n'
vectors = [(b"\x0b" * 20, b"Hi There"), (b"Jefe", b"what do ya want for nothing?"),
           (b"a" * 131, b"key larger than one SHA256 block"), (b"key", b""),
           (b"test-device-key", b"device-id17000000000000000000000000"),
           (b"test-key", "device-中文".encode("utf-8"))]
for index, (key, message) in enumerate(vectors):
    expected = base64.b64encode(hmac.new(key, message, hashlib.sha256).digest()).decode()
    for name, value in (("key", key), ("message", message)):
        code += f"const unsigned char {name}{index}[] = {{" + ",".join(str(v) for v in value) + ("," if value else "") + "0};\n"
    code += f'assert(hmac_signature_with_key((const char*)key{index}, (const char*)message{index}, output, sizeof(output)) == ESP_OK);\nassert(strcmp(output, "{expected}") == 0);\n'
    raw = hmac.new(key, message, hashlib.sha256).digest()
    code += f'const uint8_t expected{index}[32] = {{' + ','.join(str(v) for v in raw) + '};\n'
    code += f'assert(xiaotai_hmac_sha256(key{index}, {len(key)}, message{index}, {len(message)}, digest) == ESP_OK);\n'
    code += f'assert(memcmp(digest, expected{index}, 32) == 0);\n'
for index, message in enumerate((b"", b"abc", bytes(range(256)), b"a" * 4097)):
    raw = hashlib.sha256(message).digest()
    code += f'const uint8_t image{index}[] = {{' + ','.join(str(v) for v in message) + (',' if message else '') + '0};\n'
    code += f'const uint8_t hash{index}[32] = {{' + ','.join(str(v) for v in raw) + '};\n'
    code += f'assert(verify_image_sha256(image{index}, {len(message)}, hash{index}) == ESP_OK);\n'
    code += f'memcpy(digest, hash{index}, 32); digest[31] ^= 1;\n'
    code += f'assert(verify_image_sha256(image{index}, {len(message)}, digest) == ESP_ERR_INVALID_CRC);\n'
    if message:
        code += f'assert(verify_image_sha256(image{index}, {len(message)-1}, hash{index}) == ESP_ERR_INVALID_CRC);\n'
code += '''assert(hmac_signature_with_key(NULL, "text", output, sizeof(output)) == ESP_ERR_INVALID_ARG);
assert(hmac_signature_with_key("", "text", output, sizeof(output)) == ESP_ERR_INVALID_ARG);
assert(hmac_signature_with_key("key", NULL, output, sizeof(output)) == ESP_ERR_INVALID_ARG);
assert(hmac_signature_with_key("key", "text", NULL, sizeof(output)) == ESP_ERR_INVALID_ARG);
assert(hmac_signature_with_key("key", "text", output, 0) == ESP_ERR_INVALID_ARG);
assert(hmac_signature_with_key("key", "text", output, 4) == ESP_ERR_INVALID_SIZE);
assert(output[0] == '\\0');
assert(xiaotai_hmac_sha256(NULL, 3, "text", 4, digest) == ESP_ERR_INVALID_ARG);
assert(xiaotai_hmac_sha256("key", 0, "text", 4, digest) == ESP_ERR_INVALID_ARG);
assert(xiaotai_hmac_sha256("key", 3, NULL, 4, digest) == ESP_ERR_INVALID_ARG);
assert(xiaotai_hmac_sha256("key", 3, "text", 4, NULL) == ESP_ERR_INVALID_ARG);
assert(xiaotai_hmac_sha256("key", 3, NULL, 0, digest) == ESP_OK);
assert(verify_image_sha256(NULL, 1, digest) == ESP_ERR_INVALID_CRC);
assert(verify_image_sha256((const uint8_t*)"abc", 3, NULL) == ESP_ERR_INVALID_CRC);
for (int i=0; i<4096; ++i) {
    assert(hmac_signature_with_key("key", "text", output, sizeof(output)) == ESP_OK);
    assert(xiaotai_hmac_sha256("key", 3, "text", 4, digest) == ESP_OK);
}
puts("PSA HMAC: both production functions passed 6 reference vectors, invalid inputs and 4096 key lifecycles");
puts("C61 image SHA256: 4 reference vectors, checksum mismatch, truncated data and invalid inputs passed");
return 0; }
'''
fixture.write_text(code, encoding="utf-8")
subprocess.run(["cmake", "-S", str(ROOT / "tests/host_crypto"), "-B", str(out),
                f"-DPSA_SOURCE={source}", f"-DFIXTURE={fixture}", "-DCMAKE_BUILD_TYPE=Debug"], check=True)
subprocess.run(["cmake", "--build", str(out), "--parallel", "4"], check=True)
subprocess.run([str(out / "platform_hmac")], check=True)
