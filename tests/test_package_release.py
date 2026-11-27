"""Release packaging: merged images, manifest, checksums and the gates that refuse a bad build."""
# Replaces legacy: tests/test_package_release.py
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import image_trailer  # noqa: E402
from flash_layout import flash_layout  # noqa: E402
import package_release as release  # noqa: E402

PRODUCT = "xiaotai-demo-c3"
APP = PRODUCT.replace("-", "_")  # SDK builds name the image after the product
WORK = "/work"  # where the recorded build ran
CHANGELOG = "# 版本记录\n\n## 0.2.0（未发布）\n\n### 功能\n\n- 演示功能。\n\n## 0.1.0\n\n- 基线。\n"
# What the board package says its downloads are called (board.json identity.release).
RELEASE = {"slug": "demo-esp32c3", "model": "演示开发板 · ESP32-C3", "file": "demo-c3"}
IDF_VERSIONS = {"6.1": "6.1.0", "6.0": "6.0.3", "5.5": "5.5.5"}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def image(chip, size_code, body=b"", minimum=3, maximum=199):
    header = bytearray(24)
    header[0], header[1], header[3] = 0xE9, 1, size_code << 4
    struct.pack_into("<HBHH", header, 12, chip, 0, minimum, maximum)
    return bytes(header) + body


def app_desc(version, elf_sha):
    desc = bytearray(256)
    struct.pack_into("<I", desc, 0, release.APP_DESC_MAGIC)
    desc[16:16 + len(version)] = version.encode()
    desc[48:55] = b"xiaotai"
    desc[112:118] = b"v6.0.2"
    desc[144:176] = bytes.fromhex(elf_sha)
    return bytes(8) + bytes(desc)  # first segment header, then the description


def partition_table(nvs_offset=0x9000, app_size=0x100000):
    rows = b""
    for kind, subtype, offset, size, label in ((1, 2, nvs_offset, 0x6000, b"nvs"),
                                               (0, 0, 0x10000, app_size, b"factory")):
        rows += release.PARTITION.pack(b"\xaa\x50", kind, subtype, offset, size, label.ljust(16, b"\0"), 0)
    rows += b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(rows).digest()
    return rows + b"\xff" * (0xC00 - len(rows))


class PackageReleaseTests(unittest.TestCase):
    def setUp(self):
        work = tempfile.TemporaryDirectory()
        self.addCleanup(work.cleanup)
        self.base = Path(work.name).resolve()
        self.repo = self.base / "repo"
        self.write_source("CHANGELOG.md", CHANGELOG)
        self.write_source(f"products/{PRODUCT}/product.json", json.dumps({"name": PRODUCT, "board": "vendor/demo"}))
        self.write_source("sdk/app.c", "int main(void) { return 0; }\n")
        self.write_board(RELEASE)
        self.write_source("components/tirtc_sdk/packages.json", json.dumps({"default_line": "6.0", "release_line": "6.1"}))
        subprocess.run(["git", "init", "-q", str(self.repo)], check=True)
        self.commit("source")
        self.build = self.base / "build"
        self.make_build()

    def write_source(self, name, content):
        path = self.repo / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content.encode("utf-8"))

    def write_board(self, names):
        identity = {"title": "演示板", "profile_model": "Demo-C3", **({} if names is None else {"release": names})}
        self.write_source("boards/vendor/demo/board.json", json.dumps({"identity": identity}, ensure_ascii=False))

    def commit(self, message):
        git = ["git", "-C", str(self.repo), "-c", "core.autocrlf=false",
               "-c", "user.name=test", "-c", "user.email=test@example.invalid"]
        subprocess.run([*git, "add", "-A"], check=True)
        subprocess.run([*git, "commit", "-q", "-m", message], check=True)

    def make_build(self, version="0.2.0-rc.1", elf=b"ELF demo", inputs=None, boot_size_code=2, folder=None,
                   line="6.1", product=PRODUCT, board="vendor/demo"):
        folder = self.build if folder is None else folder
        app = product.replace("-", "_")  # SDK builds name the image after the product
        files = {
            "bootloader/bootloader.bin": image(0x05, boot_size_code, b"boot"),
            "partition_table/partition-table.bin": partition_table(),
            f"{app}.bin": image(0x05, 2, app_desc(version, sha(elf)) + b"app body"),
            f"{app}.elf": elf,
        }
        for name, data in files.items():
            (folder / name).parent.mkdir(parents=True, exist_ok=True)
            (folder / name).write_bytes(data)
        args = {"flash_settings": {"flash_mode": "dio", "flash_size": "4MB", "flash_freq": "80m"},
                "flash_files": {"0x0": "bootloader/bootloader.bin", "0x8000": "partition_table/partition-table.bin",
                                "0x10000": f"{app}.bin"},
                "bootloader": {"offset": "0x0", "file": "bootloader/bootloader.bin"},
                "partition-table": {"offset": "0x8000", "file": "partition_table/partition-table.bin"},
                "app": {"offset": "0x10000", "file": f"{app}.bin"},
                "extra_esptool_args": {"chip": "esp32c3"}}
        (folder / "flasher_args.json").write_text(json.dumps(args), encoding="utf-8")
        if inputs is None:
            inputs = {f"{WORK}/sdk/app.c": sha((self.repo / "sdk/app.c").read_bytes()),
                      f"{WORK}/products/{product}/product.json": sha((self.repo / f"products/{product}/product.json").read_bytes()),
                      f"{WORK}/managed_components/x/x.c": sha(b"generated"),
                      "/opt/esp-idf/components/x.c": sha(b"idf")}
        raw = (json.dumps(inputs, indent=2) + "\n").encode()
        (folder / "build-inputs.json").write_bytes(raw)
        record = {"entry": "sdk", "name": product, "board": board, "target": "esp32c3", "stage": "firmware", "idf_line": line,
                  "build_dir": f"{WORK}/build/{product}/sdk-firmware",
                  "config": {"CONFIG_PARTITION_TABLE_CUSTOM_FILENAME": f'"boards/{board}/partitions.csv"'},
                  "flash_layout": flash_layout(folder, "esp32c3", app_name=f"{app}.bin"), "input_manifest_sha256": sha(raw),
                  "idf_identity": {"version": IDF_VERSIONS[line], "git_commit": "abc"},
                  "sdk_sha256": "s" * 64,
                  }
        (folder / "build-record.json").write_text(json.dumps(record), encoding="utf-8")

    def package(self, version="0.2.0-rc.1", out="out", levels=None, **options):
        return release.package([self.build], version, self.base / out, {PRODUCT: "B"} if levels is None else levels,
                               release.Commit(self.repo, "HEAD"), **options)

    def refused(self, pattern, **arguments):
        with self.assertRaisesRegex(release.ReleaseError, pattern):
            self.package(**arguments)

    def test_data_behind_the_image_is_part_of_the_release(self):
        # A whole image: one segment that holds the description, padded to its checksum.
        body = app_desc("0.2.0-rc.1", sha(b"ELF demo"))[8:] + b"app body"
        app = image(0x05, 2, struct.pack("<II", 0x42000000, len(body)) + body)
        app += bytes(15 - len(app) % 16) + b"!"
        carried = image_trailer.attached(app, b"pictures" * 500)
        (self.build / f"{APP}.bin").write_bytes(carried)
        path = self.build / "build-record.json"
        record = json.loads(path.read_text(encoding="utf-8"))
        record["flash_layout"] = flash_layout(self.build, "esp32c3", app_name=f"{APP}.bin")
        record["image_trailer"] = image_trailer.describe(carried, "demo.pack")
        path.write_text(json.dumps(record), encoding="utf-8")
        self.package()
        full = (self.base / "out" / "v0.2.0-rc.1_demo-c3_merged.bin").read_bytes()
        self.assertEqual(full[0x10000:], carried)
        record["image_trailer"]["sha256"] = "0" * 64
        path.write_text(json.dumps(record), encoding="utf-8")
        self.refused("does not match its record", out="again")

    def test_release_files_and_manifest(self):
        manifest = self.package(elf=True)
        out = self.base / "out"
        stem = "v0.2.0-rc.1_demo-c3"
        full = (out / f"{stem}_merged.bin").read_bytes()
        boot = (self.build / "bootloader/bootloader.bin").read_bytes()
        table = (self.build / "partition_table/partition-table.bin").read_bytes()
        app = (self.build / f"{APP}.bin").read_bytes()
        self.assertEqual(full[:len(boot)], boot)
        self.assertEqual(full[len(boot):0x8000], b"\xff" * (0x8000 - len(boot)))
        self.assertEqual(full[0x8000:0x8000 + len(table)], table)
        # Merged up to the end of the app, not padded to the flash size.
        self.assertEqual(full[0x10000:], app)
        self.assertEqual(sorted(p.name for p in out.glob("*.bin")), [f"{stem}_merged.bin"])
        self.assertEqual((out / f"debug/{stem}.elf").read_bytes(), b"ELF demo")

        sums = (out / "SHA256SUMS.txt").read_text(encoding="utf-8").splitlines()
        listed = {line.split("  ", 1)[1]: line.split("  ", 1)[0] for line in sums}
        on_disk = {p.relative_to(out).as_posix() for p in out.rglob("*") if p.is_file()} - {"SHA256SUMS.txt"}
        on_disk.remove(f"debug/{stem}.elf")  # kept locally, not an attachment
        self.assertEqual(set(listed), on_disk)
        for name, digest in listed.items():
            self.assertEqual(sha((out / name).read_bytes()), digest, name)

        written = json.loads((out / "release-manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(written, manifest)
        self.assertTrue(manifest["prerelease"])
        product = manifest["products"][0]
        self.assertEqual(product["level"], "B")
        self.assertEqual((manifest["schema"], manifest["idf_line"]), (2, "6.1"))
        self.assertEqual((product["title"], product["board_model"]), ("演示板", "Demo-C3"))
        self.assertEqual((product["slug"], product["model"]), ("demo-esp32c3", "演示开发板 · ESP32-C3"))
        self.assertEqual(product["files"]["merged"]["label"], "演示开发板 · ESP32-C3 固件 v0.2.0-rc.1")
        self.assertTrue(product["files"]["merged"]["name"].isascii())
        self.assertEqual(product["chip_revision"], {"min": "v0.3", "max": "v1.99", "min_full": 3, "max_full": 199})
        self.assertEqual(product["partition_table"]["name"], "boards/vendor/demo/partitions.csv")
        self.assertEqual(product["partition_table"]["app"], {"label": "factory", "offset": "0x10000", "size": 0x100000})
        self.assertTrue(product["full_image_erases_nvs"])
        self.assertEqual(product["files"]["merged"]["offset"], "0x0")
        self.assertEqual(product["app"], {"offset": "0x10000", "bytes": len(app), "sha256": sha(app)})
        self.assertEqual(product["partition_table"]["sha256"], sha(table))
        self.assertEqual(product["source_check"], {"inputs_matched": 2, "inputs_downloaded": 0,
                                                   "inputs_outside_source": 1})
        guide = (out / release.guide_name("0.2.0-rc.1")).read_text(encoding="utf-8")
        self.assertIn(f"write-flash 0x0 {stem}_merged.bin", guide)
        self.assertIn("会清掉配网和绑定", guide)
        self.assertIn("## 演示开发板 · ESP32-C3\n", guide)
        notes = (out / release.notes_name("0.2.0-rc.1")).read_text(encoding="utf-8")
        self.assertIn("演示功能", notes)
        self.assertIn("统一用 ESP-IDF 6.1.0 编译", notes)
        self.assertIn(f"| 演示开发板 · ESP32-C3 | Demo-C3 | esp32c3 | `{stem}_merged.bin` |", notes)
        self.assertTrue(all(name.isascii() for name in listed), listed)

    def test_downloaded_package_must_be_the_pinned_official_one(self):
        folder = "tirtc__demo__v1"
        library, header = b"official library", b"official header"
        self.write_source("components/tirtc_sdk/packages.json", json.dumps({
            "default_line": "6.0", "release_line": "6.1", "lines": {"6.1": {"packages": {"generic": {
                "path": f"demo/{folder}", "sha256": "a" * 64, "contract": "contracts/6.1/demo.json"}}}}}))
        self.write_source("components/tirtc_sdk/contracts/6.1/demo.json", json.dumps(
            {"sha256": sha(library), "headers": {"tiRTC.h": sha(header)}}))
        self.commit("pin a downloaded package")
        package = f"{WORK}/components/tirtc_sdk/packages/{folder}"
        inputs = {f"{WORK}/sdk/app.c": sha((self.repo / "sdk/app.c").read_bytes()),
                  f"{package}/lib/libTiRTC.a": sha(library), f"{package}/include/tirtc/tiRTC.h": sha(header)}
        self.make_build(inputs=inputs)
        self.assertEqual(self.package()["products"][0]["source_check"],
                         {"inputs_matched": 1, "inputs_downloaded": 2, "inputs_outside_source": 0})
        self.make_build(inputs=dict(inputs, **{f"{package}/lib/libTiRTC.a": sha(b"another library")}))
        self.refused("differs from the commit: components/tirtc_sdk/packages/tirtc__demo__v1/lib/libTiRTC.a",
                     out="changed")
        self.make_build(inputs=dict(inputs, **{f"{package}/lib/extra.a": sha(b"extra")}))
        self.refused("not in the commit: components/tirtc_sdk/packages/tirtc__demo__v1/lib/extra.a", out="extra")

    def test_one_image_per_board_from_the_release_line(self):
        # A build on any other line is refused: the file name carries no line to tell two apart.
        for line in ("6.0", "5.5"):
            self.make_build(line=line)
            self.refused(f"Releases are built on ESP-IDF 6.1 .release_line in .*{PRODUCT} .ESP-IDF {line}.", out=f"line-{line}")
        self.make_build()
        with self.assertRaisesRegex(release.ReleaseError, "Same product given twice"):
            release.package([self.build, self.build], "0.2.0-rc.1", self.base / "again", {PRODUCT: "A"},
                            release.Commit(self.repo, "HEAD"))
        # Without release_line the repository's default line is the release line.
        self.write_source("components/tirtc_sdk/packages.json", json.dumps({"default_line": "6.0"}))
        self.commit("no release line")
        self.refused("Releases are built on ESP-IDF 6.0 .default_line in", out="default")
        self.make_build(line="6.0")
        self.assertEqual(self.package(out="default")["idf_line"], "6.0")

    def second_board(self, names, product="xiaotai-other-s3", board="vendor/other"):
        """Another product, on another board unless told otherwise, built in its own folder."""
        self.write_source(f"products/{product}/product.json", json.dumps({"name": product, "board": board}))
        if board != "vendor/demo":
            self.write_source(f"boards/{board}/board.json", json.dumps(
                {"identity": {"title": "另一块板", "profile_model": "Other-S3", "release": names}}, ensure_ascii=False))
        self.commit(f"{product} on {board}")
        folder = self.base / f"build-{product}"
        inputs = {f"{WORK}/sdk/app.c": sha((self.repo / "sdk/app.c").read_bytes()),
                  f"{WORK}/products/{product}/product.json": sha((self.repo / f"products/{product}/product.json").read_bytes())}
        self.make_build(folder=folder, inputs=inputs, product=product, board=board)
        return folder

    def test_several_boards_in_one_release(self):
        other = self.second_board({"slug": "other-esp32s3", "model": "另一块开发板 · ESP32-S3", "file": "other-s3"})
        levels = {PRODUCT: "A", "xiaotai-other-s3": "B"}
        packages = {}
        for out, builds in (("one", [self.build, other]), ("two", [other, self.build])):
            manifest = release.package(builds, "0.2.0-rc.1", self.base / out, levels, release.Commit(self.repo, "HEAD"))
            # Products come out by name, whatever order the folders were given in.
            self.assertEqual([p["name"] for p in manifest["products"]], [PRODUCT, "xiaotai-other-s3"])
            self.assertEqual([p["files"]["merged"]["name"] for p in manifest["products"]],
                             ["v0.2.0-rc.1_demo-c3_merged.bin", "v0.2.0-rc.1_other-s3_merged.bin"])
            packages[out] = {p.relative_to(self.base / out): p.read_bytes()
                             for p in (self.base / out).rglob("*") if p.is_file()}
        self.assertEqual(packages["one"], packages["two"])
        notes = (self.base / "one" / release.notes_name("0.2.0-rc.1")).read_text(encoding="utf-8")
        self.assertIn("| 演示开发板 · ESP32-C3 | Demo-C3 | esp32c3 | `v0.2.0-rc.1_demo-c3_merged.bin` |", notes)
        self.assertIn("| 另一块开发板 · ESP32-S3 | Other-S3 | esp32c3 | `v0.2.0-rc.1_other-s3_merged.bin` |", notes)
        guide = (self.base / "one" / release.guide_name("0.2.0-rc.1")).read_text(encoding="utf-8")
        self.assertLess(guide.index("## 演示开发板 · ESP32-C3"), guide.index("## 另一块开发板 · ESP32-S3"))

    def test_one_download_per_name(self):
        # Two boards under one file name would overwrite each other; under one slug a download page could
        # not tell them apart.
        for index, (names, key) in enumerate(((dict(RELEASE, slug="other-esp32s3"), "file"),
                                              (dict(RELEASE, file="other-s3"), "slug"))):
            other = self.second_board(names)
            with self.assertRaisesRegex(release.ReleaseError,
                                        f"identity.release.{key} is used by more than one product: "
                                        f".*{PRODUCT} .vendor/demo.*xiaotai-other-s3 .vendor/other"):
                release.package([self.build, other], "0.2.0-rc.1", self.base / f"shared-{index}", {},
                                release.Commit(self.repo, "HEAD"), allow_build_only=True)
        # A board has one download: two of its products cannot both be released.
        twin = self.second_board(None, product="xiaotai-demo-twin", board="vendor/demo")
        with self.assertRaisesRegex(release.ReleaseError, "identity.release.slug is used by more than one product"):
            release.package([self.build, twin], "0.2.0-rc.1", self.base / "twin", {},
                            release.Commit(self.repo, "HEAD"), allow_build_only=True)

    def test_board_names_its_downloads(self):
        for names, message in ((None, "identity.release .* a board without it is not released"),
                               ("demo-c3", "identity.release .* a board without it is not released"),
                               (dict(RELEASE, file="Demo_C3"), "identity.release.file is lower-case"),
                               (dict(RELEASE, slug="演示"), "identity.release.slug is lower-case"),
                               (dict(RELEASE, model=""), "identity.release.model is the name readers see"),
                               (dict(RELEASE, model=" 演示开发板"), "identity.release.model is the name readers see"),
                               (dict(RELEASE, model="演示\n开发板"), "identity.release.model is the name readers see")):
            self.write_board(names)
            self.commit("names")
            self.refused(message, out="refused")
        self.write_board(dict(RELEASE, slug="waveshare-p4-3.5", file="waveshare-esp32p4-3.5"))
        self.commit("a dot in the name")
        product = self.package()["products"][0]
        self.assertEqual((product["slug"], product["files"]["merged"]["name"]),
                         ("waveshare-p4-3.5", "v0.2.0-rc.1_waveshare-esp32p4-3.5_merged.bin"))

    def test_boards_of_this_repository(self):
        # Every board that names its downloads does so validly, and no two share a name.
        lines = json.loads((ROOT / "components/tirtc_sdk/packages.json").read_text(encoding="utf-8"))
        self.assertIn(lines["release_line"], lines["lines"])
        taken = {"slug": {}, "file": {}}
        for path in sorted((ROOT / "boards").glob("*/*/board.json")):
            board = json.loads(path.read_text(encoding="utf-8"))
            declared = board.get("identity", {}).get("release")
            if declared is None:
                continue
            names = release.release_names(declared, path.relative_to(ROOT).as_posix())
            for key in taken:
                self.assertNotIn(names[key], taken[key], path)
                taken[key][names[key]] = path
        self.assertTrue(taken["file"])

    def test_same_inputs_same_package(self):
        self.package(out="one")
        self.package(out="two")
        one = {p.relative_to(self.base / "one"): p.read_bytes() for p in (self.base / "one").rglob("*") if p.is_file()}
        two = {p.relative_to(self.base / "two"): p.read_bytes() for p in (self.base / "two").rglob("*") if p.is_file()}
        self.assertEqual(one, two)

    def test_firmware_version_must_be_the_release(self):
        self.refused("firmware says version '0.2.0-rc.1', release is '0.2.0-rc.2'", version="0.2.0-rc.2")
        self.refused("is not X.Y.Z", version="v0.2")

    def test_final_release_needs_a_released_changelog(self):
        self.make_build(version="0.2.0")
        self.refused("still marks 0.2.0 as unreleased", version="0.2.0")

    def test_build_must_come_from_the_commit(self):
        self.write_source("sdk/app.c", "int main(void) { return 1; }\n")
        self.commit("changed after the build")
        self.refused("differs from the commit: sdk/app.c")

    def test_build_input_missing_from_the_commit(self):
        inputs = {f"{WORK}/sdk/local_only.c": sha(b"x")}
        self.make_build(inputs=inputs)
        self.refused("not in the commit: sdk/local_only.c")

    def test_files_changed_after_the_build(self):
        (self.build / f"{APP}.bin").write_bytes(image(0x05, 2, app_desc("0.2.0-rc.1", sha(b"ELF demo")) + b"other"))
        self.refused("flash files changed after the build")

    def test_elf_must_belong_to_the_app(self):
        self.make_build()
        (self.build / f"{APP}.elf").write_bytes(b"another ELF")
        self.refused(f"{APP}.elf is not the ELF of {APP}.bin")

    def test_bootloader_flash_size_must_match(self):
        self.make_build(boot_size_code=4)
        self.refused("bootloader header flash size does not match 4MB")

    def test_partition_table_checksum(self):
        path = self.build / "partition_table/partition-table.bin"
        broken = bytearray(path.read_bytes())
        broken[20] ^= 1
        path.write_bytes(bytes(broken))
        record = json.loads((self.build / "build-record.json").read_text(encoding="utf-8"))
        record["flash_layout"] = flash_layout(self.build, "esp32c3", app_name=f"{APP}.bin")
        (self.build / "build-record.json").write_text(json.dumps(record), encoding="utf-8")
        self.refused("Partition table checksum is wrong")

    def test_only_verified_products_unless_trial(self):
        self.refused("build-only: \\['xiaotai-demo-c3'\\]", levels={})
        self.refused("names products that were not given", levels={PRODUCT: "A", "xiaotai-other": "A"})
        manifest = self.package(levels={}, allow_build_only=True)
        self.assertEqual(manifest["products"][0]["level"], "C")

    def test_output_folder_must_be_empty(self):
        (self.base / "out").mkdir()
        (self.base / "out/old.bin").write_bytes(b"x")
        self.refused("is not empty")

    def test_product_must_belong_to_the_board(self):
        self.write_source(f"products/{PRODUCT}/product.json", json.dumps({"name": PRODUCT, "board": "vendor/other"}))
        self.commit("moved")
        self.make_build()
        self.refused("is not a product of vendor/demo")


if __name__ == "__main__":
    unittest.main()
