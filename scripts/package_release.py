"""Package finished XiaoTai firmware builds into a release folder.

For every build folder (the firmware output of scripts/build.py) this checks that the
build came from the release commit and that its images agree with each other, then writes
one complete image per product and ESP-IDF line, the way the BK7258 line of the firmware
names its downloads (version first, then the board, the variant and the image type):

  v<version>_<product>_idf<line>_merged.bin   bootloader, partition table and app merged
                                              from 0x0 up to the end of the last of them

<product> is the product name without "xiaotai-" and <line> the ESP-IDF line
("6.0", "5.5"), so every file says what it is. A product may be given once per line.
The image ends where its last part ends: the flash size is not padded out.
  release-manifest.json                   identity of every product and file
  xiaotai-v<version>-flashing-guide.md    flashing guide (Chinese)
  xiaotai-v<version>-release-notes.md     release text (Chinese)

Every file name is ASCII: GitHub renames release attachments with other characters.
  SHA256SUMS.txt                          checksums of everything above

Nothing is flashed and nothing leaves this machine. The output is the same for the same
inputs: no timestamps, sorted keys.

Usage:
  python scripts/package_release.py --version 0.2.0-rc.1 --out <empty dir> \
      --level xiaotai-xmini-c3-v3=A --level xiaotai-cores3-lite=B <build dir>...
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath, PureWindowsPath
import re
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from flash_layout import flash_layout  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
VERSION = re.compile(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-(rc\.[1-9][0-9]*|dev))?")
LEVELS = {
    "A": "实机回归",
    "B": "代表板验证加本板构建",
    "C": "仅构建",
}
# esp_chip_id_t values written into every ESP image header.
CHIP_IDS = {"esp32": 0x00, "esp32s2": 0x02, "esp32c3": 0x05, "esp32s3": 0x09, "esp32c2": 0x0C,
            "esp32c6": 0x0D, "esp32h2": 0x10, "esp32p4": 0x12, "esp32c61": 0x14, "esp32c5": 0x17,
            "esp32s31": 0x20}
# High nibble of image header byte 3, as esptool writes it for these chips.
FLASH_SIZE_CODES = {"1MB": 0, "2MB": 1, "4MB": 2, "8MB": 3, "16MB": 4, "32MB": 5, "64MB": 6, "128MB": 7}
# esp_image_header_t (24 bytes) up to max_chip_rev_full.
IMAGE_HEADER = struct.Struct("<BBBB4xxxxxHBHH")
APP_DESC_OFFSET = 0x20  # image header + first segment header
APP_DESC_MAGIC = 0xABCD5432
PARTITION = struct.Struct("<2sBBII16sI")
DATA_SUBTYPES = {0x00: "ota", 0x01: "phy", 0x02: "nvs", 0x03: "coredump", 0x04: "nvs_keys", 0x05: "efuse",
                 0x81: "fat", 0x82: "spiffs", 0x83: "littlefs"}
GENERATED_ROOTS = {"build", "managed_components"}


class ReleaseError(ValueError):
    pass


def check(condition, message):
    if not condition:
        raise ReleaseError(message)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def text(field):
    return field.split(b"\0", 1)[0].decode("utf-8", "replace")


def revision(full):
    return f"v{full // 100}.{full % 100}"


def image_header(data, target, what):
    check(len(data) >= IMAGE_HEADER.size and data[0] == 0xE9, f"{what} is not an ESP image")
    _, _, mode, size_freq, chip, _, minimum, maximum = IMAGE_HEADER.unpack_from(data)
    check(target in CHIP_IDS, f"No ESP chip id known for target {target}")
    check(chip == CHIP_IDS[target], f"{what} was built for chip id {chip}, not {target}")
    return {"flash_size_code": size_freq >> 4, "min_rev_full": minimum, "max_rev_full": maximum}


def app_description(data, what):
    check(len(data) >= APP_DESC_OFFSET + 176, f"{what} is too short for an app description")
    desc = data[APP_DESC_OFFSET:APP_DESC_OFFSET + 176]
    check(struct.unpack_from("<I", desc)[0] == APP_DESC_MAGIC, f"{what} has no app description")
    return {"version": text(desc[16:48]), "project": text(desc[48:80]), "idf": text(desc[112:144]),
            "elf_sha256": desc[144:176].hex()}


def partitions(data):
    rows, end = [], None
    for at in range(0, len(data) - len(data) % 32, 32):
        entry = data[at:at + 32]
        if entry[:2] == b"\xaa\x50":
            _, kind, subtype, offset, size, label, _ = PARTITION.unpack(entry)
            rows.append({"label": text(label), "type": {0: "app", 1: "data"}.get(kind, hex(kind)),
                         "subtype": (DATA_SUBTYPES.get(subtype, hex(subtype)) if kind == 1
                                     else "factory" if subtype == 0 else hex(subtype)),
                         "offset": hex(offset), "size": size})
        elif entry[:2] == b"\xeb\xeb":
            check(entry[16:] == hashlib.md5(data[:at]).digest(), "Partition table checksum is wrong")
        elif entry == b"\xff" * 32:
            end = at
            break
        else:
            raise ReleaseError(f"Partition table has an unknown entry at {at:#x}")
    check(rows and end is not None, "Partition table is empty or not terminated")
    return rows


def git(repo, *args, data=None):
    process = subprocess.run(["git", "-C", str(repo), *args], input=data, capture_output=True)
    check(process.returncode == 0, f"git {' '.join(args)}: {process.stderr.decode(errors='replace').strip()}")
    return process.stdout


class Commit:
    """Read files of one commit; contents are fetched in one batch."""

    def __init__(self, repo, ref):
        self.repo = repo
        self.id = git(repo, "rev-parse", "--verify", f"{ref}^{{commit}}").decode().strip()
        listing = git(repo, "ls-tree", "-r", "-z", self.id).split(b"\0")
        self.blobs = {}
        for line in filter(None, listing):
            meta, path = line.split(b"\t", 1)
            mode, kind, blob = meta.split()
            if kind == b"blob":
                self.blobs[path.decode("utf-8")] = blob.decode()
        self.cache = {}

    def digests(self, paths):
        wanted = [p for p in paths if p not in self.cache]
        if wanted:
            out = git(self.repo, "cat-file", "--batch",
                      data="".join(self.blobs[p] + "\n" for p in wanted).encode())
            at = 0
            for path in wanted:
                header_end = out.index(b"\n", at)
                size = int(out[at:header_end].split()[2])
                body = out[header_end + 1:header_end + 1 + size]
                self.cache[path] = sha256(body)
                at = header_end + 2 + size
        return {p: self.cache[p] for p in paths}

    def read(self, path):
        check(path in self.blobs, f"{path} is not in commit {self.id[:12]}")
        return git(self.repo, "cat-file", "blob", self.blobs[path])


def downloaded_packages(commit):
    """Files of the official TiRTC packages the build downloads, with the SHA-256 the commit pins.

    The archives are not in the repository: scripts/fetch_tirtc.py unpacks them into
    components/tirtc_sdk/packages/. Their library and headers must equal the contract
    that packages.json names for them in the release commit.
    """
    table = json.loads(commit.read("components/tirtc_sdk/packages.json"))
    pinned = {}
    for line in table.get("lines", {}).values():
        for entry in line.get("packages", {}).values():
            if "path" not in entry:
                continue
            folder = "components/tirtc_sdk/packages/" + entry["path"].rsplit("/", 1)[-1]
            contract = json.loads(commit.read("components/tirtc_sdk/" + entry["contract"]))
            pinned[f"{folder}/lib/libTiRTC.a"] = contract["sha256"]
            for name, digest in contract["headers"].items():
                pinned[f"{folder}/include/tirtc/{name}"] = digest
    return pinned


def source_check(record, inputs_raw, commit):
    """Every build input inside the source tree must equal the file in the release commit,
    or, for a downloaded TiRTC package, the official file the commit pins."""
    check(sha256(inputs_raw) == record.get("input_manifest_sha256"),
          f"{record['name']}: build-inputs.json does not match its build record")
    inputs = json.loads(inputs_raw)
    built = record["build_dir"]
    flavour = PureWindowsPath if re.match(r"[A-Za-z]:[\\/]", built) else PurePosixPath
    folder = flavour(built)
    check(folder.parts[-3] == "build" and folder.parts[-2] == record["name"] and folder.name.startswith("sdk-firmware"),
          f"{record['name']}: build folder {built} is not <source>/build/<product>/sdk-firmware*")
    base = folder.parents[2]
    pinned = downloaded_packages(commit)
    checked, outside, downloaded, mismatched, untracked = {}, 0, 0, [], []
    for name, digest in inputs.items():
        path = flavour(name)
        try:
            relative = path.relative_to(base).as_posix()
        except ValueError:
            outside += 1
            continue
        if relative.split("/", 1)[0] in GENERATED_ROOTS:
            continue
        if relative in pinned:
            downloaded += 1
            if digest != pinned[relative]:
                mismatched.append(relative)
        elif relative in commit.blobs:
            checked[relative] = digest
        else:
            untracked.append(relative)
    for relative, expected in commit.digests(sorted(checked)).items():
        if checked[relative] != expected:
            mismatched.append(relative)
    for path, digest in record.get("compatibility", {}).get("inputs", {}).items():
        if path not in checked:
            if path not in commit.blobs or commit.digests([path])[path] != digest:
                mismatched.append(path)
    problems = [f"differs from the commit: {p}" for p in sorted(set(mismatched))]
    problems += [f"not in the commit: {p}" for p in sorted(untracked)]
    check(not problems, f"{record['name']} was not built from commit {commit.id[:12]}:\n  "
          + "\n  ".join(problems[:20]) + (f"\n  ... {len(problems) - 20} more" if len(problems) > 20 else ""))
    return {"inputs_matched": len(checked), "inputs_downloaded": downloaded, "inputs_outside_source": outside}


def read_build(folder, version, commit):
    folder = Path(folder).resolve()
    check((folder / "build-record.json").is_file(), f"{folder}: no build-record.json")
    record = json.loads((folder / "build-record.json").read_text(encoding="utf-8"))
    name, target = record.get("name"), record.get("target")
    check(record.get("stage") == "firmware" and "flash_layout" in record,
          f"{folder}: not a finished firmware build")
    # SDK builds name the application image after the product, as scripts/xiaotai_build_native.py does.
    layout = flash_layout(folder, target, app_name=f"{str(name).replace('-', '_')}.bin")
    check(layout == record["flash_layout"], f"{name}: flash files changed after the build")

    files = {row["file"]: (folder / row["file"]).read_bytes() for row in layout["segments"]}
    args = json.loads((folder / "flasher_args.json").read_text(encoding="utf-8"))
    sections = {key: args[key]["file"] for key in ("bootloader", "partition-table", "app")}
    app = files[sections["app"]]
    header = image_header(app, target, f"{name} app")
    boot = image_header(files[sections["bootloader"]], target, f"{name} bootloader")
    size_code = FLASH_SIZE_CODES.get(layout["flash_size"])
    check(boot["flash_size_code"] == size_code,
          f"{name}: bootloader header flash size does not match {layout['flash_size']}")
    desc = app_description(app, f"{name} app")
    check(desc["version"] == version,
          f"{name}: firmware says version {desc['version']!r}, release is {version!r} (PROJECT_VER in cmake/esp-idf-product/CMakeLists.txt)")
    # The ELF carries the APP image's name (SDK builds name both after the product).
    elf_name = Path(sections["app"]).with_suffix(".elf").name
    check((folder / elf_name).is_file(), f"{name}: no {elf_name} next to {sections['app']}")
    elf = (folder / elf_name).read_bytes()
    check(sha256(elf) == desc["elf_sha256"], f"{name}: {elf_name} is not the ELF of {sections['app']}")

    table = partitions(files[sections["partition-table"]])
    app_offset = layout["app_offset"]
    owner = next((row for row in table if row["type"] == "app" and row["offset"] == app_offset), None)
    check(owner is not None, f"{name}: no app partition starts at {app_offset}")
    check(len(app) <= owner["size"], f"{name}: app ({len(app)} bytes) exceeds partition {owner['label']}")

    check((folder / "build-inputs.json").is_file(), f"{folder}: no build-inputs.json")
    source = source_check(record, (folder / "build-inputs.json").read_bytes(), commit)
    # Builds name the product's board in boards/; products/<name>/product.json declares it.
    check(record.get("entry") == "sdk", f"{name}: only SDK builds can be released")
    product = json.loads(commit.read(f"products/{name}/product.json"))
    check(product.get("name") == name and product.get("board") == record["board"],
          f"{name} is not a product of {record['board']} in commit {commit.id[:12]}")
    # The board's own model name is what readers recognise in the notes.
    board_manifest = f"boards/{record['board']}/board.json"
    model = (json.loads(commit.read(board_manifest)).get("identity", {}).get("profile_model")
             if board_manifest in commit.blobs else None) or record["board"]

    config = record.get("config", {})
    return {
        "name": name, "line": record.get("idf_line"), "board": record["board"], "model": model, "target": target,
        "record": record, "args": args,
        "layout": layout, "files": files, "sections": sections, "elf": elf, "header": header, "desc": desc,
        "partitions": table, "app_partition": owner, "source": source,
        "partition_table_name": str(config.get("CONFIG_PARTITION_TABLE_CUSTOM_FILENAME")
                                    or config.get("CONFIG_PARTITION_TABLE_FILENAME") or "").strip('"') or None,
    }


def merged(build):
    rows = build["layout"]["segments"]
    end = max(int(row["offset"], 16) + row["bytes"] for row in rows)
    image = bytearray(b"\xff" * end)
    for row in rows:
        at = int(row["offset"], 16)
        image[at:at + row["bytes"]] = build["files"][row["file"]]
    return bytes(image)


def short_name(product):
    return product[len("xiaotai-"):] if product.startswith("xiaotai-") else product


def file_stem(build, version):
    return f"v{version}_{short_name(build['name'])}_idf{build['line']}"


def product_entry(build, level, version, written, default_line):
    record, layout, header = build["record"], build["layout"], build["header"]
    stem = file_stem(build, version)
    full = merged(build)
    files = {"merged": (f"{stem}_merged.bin", "0x0", full)}
    if written.get("elf"):
        files["elf"] = (f"debug/{stem}.elf", None, build["elf"])
    entry_files = {}
    for role, (name, offset, data) in files.items():
        written["files"][name] = data
        entry_files[role] = {"name": name, "bytes": len(data), "sha256": sha256(data),
                             **({"offset": offset} if offset else {})}
    nvs = [row for row in build["partitions"] if row["subtype"] == "nvs"]
    settings = build["args"].get("flash_settings", {})
    idf = record.get("idf_identity", {})
    app = build["files"][build["sections"]["app"]]
    return {
        "name": build["name"],
        "board": build["board"],
        "model": build["model"],
        "target": build["target"],
        "default_line": build["line"] == default_line,
        "level": level,
        "chip_revision": {"min": revision(header["min_rev_full"]), "max": revision(header["max_rev_full"]),
                          "min_full": header["min_rev_full"], "max_full": header["max_rev_full"]},
        "flash": {"size": layout["flash_size"], "mode": settings.get("flash_mode"), "freq": settings.get("flash_freq")},
        "partition_table": {
            "name": build["partition_table_name"],
            "sha256": sha256(build["files"][build["sections"]["partition-table"]]),
            "app": {k: build["app_partition"][k] for k in ("label", "offset", "size")},
            "nvs": [{k: row[k] for k in ("label", "offset", "size")} for row in nvs],
            "partitions": build["partitions"],
        },
        "full_image_erases_nvs": any(int(row["offset"], 16) < len(full) for row in nvs),
        "idf": {"line": record.get("idf_line"), "version": idf.get("version"), "commit": idf.get("git_commit"),
                "app_reports": build["desc"]["idf"]},
        "tirtc_package": record.get("tirtc_package"),
        "sdk_sha256": record.get("sdk_sha256"),
        "sdkconfig_sha256": record.get("sdkconfig_sha256"),
        "dependencies_lock_sha256": record.get("dependencies_lock_sha256"),
        "input_manifest_sha256": record.get("input_manifest_sha256"),
        "elf_sha256": build["desc"]["elf_sha256"],
        "app": {"offset": layout["app_offset"], "bytes": len(app), "sha256": sha256(app)},
        "source_check": build["source"],
        "files": entry_files,
    }


def changelog_section(commit, version):
    base = version.split("-", 1)[0]
    lines = commit.read("CHANGELOG.md").decode("utf-8").splitlines()
    start = next((i for i, line in enumerate(lines)
                  if re.match(rf"## {re.escape(base)}(?![0-9.])", line)), None)
    check(start is not None, f"CHANGELOG.md has no section for {base}")
    if "-" not in version:
        check("未发布" not in lines[start], f"CHANGELOG.md still marks {base} as unreleased")
    end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith("## ")), len(lines))
    return "\n".join(lines[start + 1:end]).strip()


def erase_note(product):
    return ("会清掉配网和绑定" if product["full_image_erases_nvs"]
            else "不覆盖 `nvs`，但会覆盖镜像范围内的其他数据")


def flashing_guide(manifest):
    out = [f"# 小钛 v{manifest['version']} 烧录说明", "",
           "## 先看这一条", "",
           "每块开发板每个 ESP-IDF 版本一个完整镜像，文件名是 `v<版本>_<产品>_idf<ESP-IDF 版本>_merged.bin`。"
           "从 `0x0` 开始写，会清掉配网和绑定，写完要重新配网、重新绑定。", "",
           "## 准备", "",
           "1. 安装 esptool：`pip install esptool`。下面的命令是 esptool 5 的写法；esptool 4 把 `write-flash` 写成 `write_flash`。",
           "2. 用 USB 线把板子接到电脑，找到串口：Windows 是 `COM3` 这样的名字，Linux 是 `/dev/ttyACM0` 或 `/dev/ttyUSB0`，macOS 是 `/dev/cu.usbmodem…`。",
           "3. 核对下载的文件：Linux 和 macOS 运行 `sha256sum -c SHA256SUMS.txt`；Windows 在 PowerShell 里运行 "
           "`Get-FileHash <文件名>`，和 `SHA256SUMS.txt` 里的值比对。", ""]
    for product in manifest["products"]:
        files = product["files"]
        port = "<串口>"
        flash = f"esptool --chip {product['target']} -p {port} -b 460800 write-flash"
        out += [f"## {product['model']}（ESP-IDF {product['idf']['version']}）", "",
                f"产品 `{product['name']}`，板卡 `{product['board']}`，芯片 {product['target']}"
                f"（芯片版本 {product['chip_revision']['min']} 到 {product['chip_revision']['max']}），"
                f"Flash {product['flash']['size']}，验证等级 {product['level']}（{LEVELS[product['level']]}）。"
                f"镜像 {files['merged']['bytes'] / 1048576:.1f} MB，{erase_note(product)}：", "",
                "```bash", f"{flash} 0x0 {files['merged']['name']}", "```", ""]
    return "\n".join(out)


def guide_name(version):
    return f"xiaotai-v{version}-flashing-guide.md"


def notes_name(version):
    return f"xiaotai-v{version}-release-notes.md"


def release_notes(manifest, changes):
    out = [f"# 小钛 v{manifest['version']}", "", changes, "", "## 固件", ""]
    lines = {product["idf"]["line"]: product["default_line"] for product in manifest["products"]}
    if len(lines) > 1:
        default = sorted((line for line, is_default in lines.items() if is_default), reverse=True)
        others = sorted((line for line, is_default in lines.items() if not is_default), reverse=True)
        out += [f"开发板按 ESP-IDF 版本各有一份固件，文件名里的 `idf{'`、`idf'.join(default + others)}` 标明用哪个版本编译，"
                f"功能相同。一般用 `idf{'`、`idf'.join(default)}`；其他版本方便和自己基于该版本的工程对齐。", ""]
    out += ["| 开发板 | 产品 | 芯片 | ESP-IDF | 验证等级 | 完整镜像 | 大小 |",
            "| --- | --- | --- | --- | --- | --- | --- |"]
    for product in manifest["products"]:
        merged_file = product["files"]["merged"]
        out.append(f"| {product['model']} | `{product['name']}` | {product['target']} | {product['idf']['version']} | "
                   f"{product['level']}（{LEVELS[product['level']]}） | `{merged_file['name']}` | "
                   f"{merged_file['bytes'] / 1048576:.1f} MB |")
    out += ["", "完整镜像从 `0x0` 写入，会清掉配网和绑定，烧录方法和注意事项见 "
            f"`{guide_name(manifest['version'])}`（烧录说明），文件校验和见 `SHA256SUMS.txt`。",
            f"源码提交 `{manifest['source']['commit']}`。", ""]
    return "\n".join(out)


def package(builds, version, out, levels, commit, elf=False, allow_build_only=False):
    match = VERSION.fullmatch(version)
    check(match, f"Version {version!r} is not X.Y.Z, X.Y.Z-rc.N or X.Y.Z-dev")
    out = Path(out)
    check(not out.exists() or not any(out.iterdir()), f"{out} is not empty")
    loaded = [read_build(folder, version, commit) for folder in builds]
    names = [build["name"] for build in loaded]
    keys = [(build["name"], build["line"]) for build in loaded]
    check(len(set(keys)) == len(keys),
          f"Same product given twice for one ESP-IDF line: {sorted({k for k in keys if keys.count(k) > 1})}")
    default_line = json.loads(commit.read("components/tirtc_sdk/packages.json"))["default_line"]
    unknown = sorted(set(levels) - set(names))
    check(not unknown, f"--level names products that were not given: {unknown}")
    build_only = sorted(name for name in names if levels.get(name, "C") == "C")
    check(allow_build_only or not build_only,
          f"Only verified (A or B) firmware is released; build-only: {build_only}. "
          "Pass --allow-build-only for a local trial package.")
    written = {"files": {}, "elf": elf}
    products = [product_entry(build, levels.get(build["name"], "C"), version, written, default_line)
                for build in sorted(loaded, key=lambda b: (b["name"], b["line"] != default_line, b["line"] or ""))]
    manifest = {"schema": 1, "version": version, "tag": f"v{version}",
                "prerelease": match[4] is not None,
                "source": {"commit": commit.id}, "levels": LEVELS, "products": products}
    texts = {
        "release-manifest.json": json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
        guide_name(version): flashing_guide(manifest),
        notes_name(version): release_notes(manifest, changelog_section(commit, version)),
    }
    for name, content in texts.items():
        written["files"][name] = content.encode("utf-8")
    # Checksums cover the attachments; debug/ stays with whoever packaged (its hash is in the manifest).
    sums = "".join(f"{sha256(data)}  {name}\n" for name, data in sorted(written["files"].items())
                   if not name.startswith("debug/"))
    written["files"]["SHA256SUMS.txt"] = sums.encode("utf-8")
    out.mkdir(parents=True, exist_ok=True)
    for name, data in written["files"].items():
        path = out / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    return manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("builds", nargs="+", type=Path, help="firmware build folders (build/<product>/sdk-firmware)")
    parser.add_argument("--version", required=True, help="X.Y.Z, X.Y.Z-rc.N or X.Y.Z-dev; must equal PROJECT_VER")
    parser.add_argument("--out", required=True, type=Path, help="new or empty output folder")
    parser.add_argument("--repo", type=Path, default=ROOT, help="source repository (default: this one)")
    parser.add_argument("--commit", default="HEAD", help="release commit the builds must come from")
    parser.add_argument("--level", action="append", default=[], metavar="PRODUCT=A|B|C",
                        help="validation level per product; products not named are C (build only)")
    parser.add_argument("--allow-build-only", action="store_true", help="also package level C products")
    parser.add_argument("--elf", action="store_true",
                        help="keep the ELF files under debug/ for crash decoding (not an attachment)")
    args = parser.parse_args(argv)
    levels = {}
    for item in args.level:
        name, _, level = item.partition("=")
        if level not in LEVELS or not name:
            parser.error(f"--level {item}: expected PRODUCT=A, PRODUCT=B or PRODUCT=C")
        levels[name] = level
    try:
        manifest = package(args.builds, args.version, args.out, levels, Commit(args.repo, args.commit),
                           elf=args.elf, allow_build_only=args.allow_build_only)
    except ReleaseError as error:
        print(f"Release check failed: {error}", file=sys.stderr)
        return 1
    for product in manifest["products"]:
        print(f"{product['name']} (ESP-IDF {product['idf']['version']}): level {product['level']}, "
              f"{product['files']['merged']['name']} "
              f"({product['source_check']['inputs_matched']} source inputs match {manifest['source']['commit'][:12]})")
    print(f"Wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
