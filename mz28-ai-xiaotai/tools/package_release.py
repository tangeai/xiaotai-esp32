"""Package one verified candidate build; refuses changed source, never publishes."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def contained(root, relative):
    path = (root / relative).resolve()
    if not path.is_relative_to(root.resolve()) or not path.is_file():
        raise ValueError(f"Invalid input path: {relative}")
    return path


def verify_full_image(path, segments, size):
    data = path.read_bytes()
    if len(data) != size:
        raise ValueError("Full image does not match physical flash size")
    end = 0
    for offset, segment in segments:
        content = segment.read_bytes()
        if offset < end or offset + len(content) > size:
            raise ValueError("Full image segment overlaps or exceeds flash")
        if data[end:offset].strip(b"\xff"):
            raise ValueError("Full image contains unexpected gap data (possibly NVS)")
        if data[offset:offset + len(content)] != content:
            raise ValueError("Full image differs from the verified build segment")
        end = offset + len(content)
    if data[end:].strip(b"\xff"):
        raise ValueError("Full image contains unexpected tail data")


def merge_full_image(path, segments):
    # Keep the verified bootloader header; merging must not modify any segment.
    command = [sys.executable, "-m", "esptool", "--chip", "esp32c3", "merge_bin",
               "--output", str(path), "--flash_mode", "keep", "--flash_freq", "keep",
               "--flash_size", "keep", "--fill-flash-size", "8MB"]
    for offset, segment in segments:
        command.extend([f"0x{offset:x}", str(segment)])
    try:
        subprocess.run(command, check=True)
    except subprocess.CalledProcessError as error:
        raise ValueError("esptool merge failed; no verified package produced") from error
    verify_full_image(path, segments, 8 * 1024 * 1024)


def validate_managed(source):
    managed = source / "managed_components"
    if not managed.exists():
        return {}
    # Use IDF's own manifest filters/hash contract, not a hand-written folder hash.
    import yaml
    from idf_component_tools.hash_tools.validate import (
        validate_hash_eq_hashfile, validate_hash_eq_hashdir,
    )
    dependencies = yaml.safe_load((source / "dependencies.lock").read_text())["dependencies"]
    expected = {name.replace("/", "__"): item for name, item in dependencies.items()
                if item["source"]["type"] == "service"}
    if {p.name for p in managed.iterdir()} != set(expected):
        raise ValueError("Unfrozen managed dependency directory")
    for name, item in expected.items():
        component = managed / name
        validate_hash_eq_hashfile(component, item["component_hash"])
        validate_hash_eq_hashdir(component, item["component_hash"])
    return {name: {"version": item["version"], "component_hash": item["component_hash"]}
            for name, item in expected.items()}


def package(candidate, output, sdk_header):
    if output.exists():
        raise ValueError("Output exists; refuse to overwrite a release")
    source = candidate / "source"
    build = source / "build"
    validation = candidate / "VALIDATION.md"
    if not validation.is_file():
        raise ValueError("Missing candidate validation report")
    manifest = json.loads((candidate / "source-manifest.json").read_text(encoding="utf-8"))
    if manifest.get("flash_size") != "8MB" or manifest.get("status") not in {"pre-release", "release"}:
        raise ValueError("Expected an MZ28 8MB release or pre-release manifest")
    for name, expected in manifest["files"].items():
        if digest(contained(source, name)) != expected:
            raise ValueError(f"Source changed after freeze: {name}")
    # Private local overrides are not in the manifest, but can silently affect a build.
    if (source / "main/app_config.local.h").exists():
        raise ValueError("Private app_config.local.h present in candidate")
    managed = validate_managed(source)
    for path in source.rglob("*"):
        relative = path.relative_to(source)
        if (path.is_file() and relative.parts[0] != "managed_components" and
                not relative.parts[0].startswith("build") and
                "__pycache__" not in relative.parts and
                relative.as_posix() not in manifest["files"]):
            raise ValueError(f"Unfrozen source input: {relative}")
    description = json.loads((build / "project_description.json").read_text(encoding="utf-8"))
    if (description["project_name"] != manifest["project_name"] or
            description["project_version"] != manifest["version"] or
            description["target"] != "esp32c3" or description["git_revision"] != "v5.5.5" or
            Path(description["project_path"]).resolve() != source.resolve()):
        raise ValueError("Build does not identify this C3/IDF5.5.5/source version")
    sdk_header_hash = digest(sdk_header)
    if digest(build / "config/sdkconfig.h") != sdk_header_hash:
        raise ValueError("SDK/application generated configuration headers differ")
    flash = json.loads((build / "flasher_args.json").read_text(encoding="utf-8"))
    if flash["extra_esptool_args"]["chip"] != "esp32c3":
        raise ValueError("Wrong flash target")
    if flash["flash_settings"] != {"flash_mode": "dio", "flash_size": manifest["flash_size"], "flash_freq": "80m"}:
        raise ValueError("Review changed flash settings before packaging")
    if set(flash["flash_files"]) != {"0x0", "0x8000", "0x10000"}:
        raise ValueError("Review changed partitions before packaging")
    entries = sorted((int(offset, 0), name) for offset, name in flash["flash_files"].items())
    rows = list(csv.reader(line for line in (source / "partitions.csv").read_text().splitlines()
                           if line.strip() and not line.lstrip().startswith("#")))
    partitions = {row[0].strip(): (int(row[3].strip(), 0), int(row[4].strip(), 0)) for row in rows}
    if partitions.get("factory", (0, 0))[0] != 0x10000 or partitions.get("nvs", (0, 0))[0] != 0x9000:
        raise ValueError("Review changed factory/NVS layout before packaging")
    boundaries = {0: 0x8000, 0x8000: partitions["nvs"][0],
                  0x10000: sum(partitions["factory"])}
    segments = []
    for offset, name in entries:
        path = contained(build, name)
        limit = boundaries[offset]
        if offset + path.stat().st_size > limit:
            raise ValueError("Flash segment overlaps its next boundary")
        segments.append((offset, path))
    output.mkdir(parents=True)
    suffix = "-pre-release" if manifest["status"] == "pre-release" else ""
    name = f'{manifest["artifact_name"]}-{manifest["version"]}{suffix}'
    with zipfile.ZipFile(output / f"{name}-source.zip", "x", zipfile.ZIP_DEFLATED) as archive:
        for relative in sorted(manifest["files"]):
            archive.write(source / relative, f"{name}/{relative}")
    firmware = output / "firmware"
    firmware.mkdir()
    for relative in ("LICENSE.txt", "docs/UPSTREAM_LICENSE.txt", "docs/TBOX_LICENSE.txt",
                     "docs/ESP_AUDIO_CODEC_LICENSE.txt", "docs/ESP_IDF_LICENSE.txt"):
        shutil.copy2(source / relative, firmware / Path(relative).name)
    args = [" ".join(flash["write_flash_args"])]
    for offset, path in segments:
        shutil.copy2(path, firmware / path.name)
        args.append(f"0x{offset:x} {path.name}")
    (firmware / "flash_args.txt").write_text("\n".join(args) + "\n", encoding="utf-8")
    full_image = firmware / f"{name}-full-8MB.bin"
    merge_full_image(full_image, segments)
    guide = (source / "docs/FLASHING.md").read_text(encoding="utf-8")
    if full_image.name not in guide or description["app_bin"] not in guide:
        raise ValueError("Flashing guide does not match the packaged filenames/version")
    (firmware / "README_FLASH.md").write_text(guide, encoding="utf-8")
    # The firmware ZIP is self-contained; its checksum file uses ZIP-local paths.
    firmware_files = sorted(path for path in firmware.iterdir() if path.is_file())
    (firmware / "SHA256SUMS.txt").write_text("".join(
        f"{digest(path)}  {path.name}\n" for path in firmware_files), encoding="utf-8")
    firmware_zip = output / f"{name}-firmware.zip"
    with zipfile.ZipFile(firmware_zip, "x", zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(firmware.iterdir()):
            archive.write(path, f"{name}-firmware/{path.name}")
    manifest["build"] = {"target": "esp32c3", "idf": "5.5.5", "app_version": manifest["version"],
                         "managed_dependencies": managed,
                         "sdkconfig_header_sha256": sdk_header_hash,
                         "app_sha256": digest(build / description["app_bin"]),
                         "elf_sha256": digest(build / description["app_elf"])}
    manifest["flash"] = [{"offset": f"0x{offset:x}", "file": f"firmware/{path.name}",
                          "bytes": path.stat().st_size, "sha256": digest(path)}
                         for offset, path in segments]
    manifest["full_image"] = {
        "offset": "0x0", "file": f"firmware/{full_image.name}",
        "bytes": full_image.stat().st_size, "sha256": digest(full_image),
        "padding": "0xff", "contains_device_credentials": False,
        "preserves_existing_nvs": False, "segment_bytes_verified": True,
    }
    manifest["firmware_archive"] = {"file": firmware_zip.name,
                                     "sha256": digest(firmware_zip)}
    shutil.copy2(validation, output / "VALIDATION.md")
    manifest["validation"] = {"file": "VALIDATION.md", "sha256": digest(validation)}
    (output / "release-manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    files = sorted(path for path in output.rglob("*") if path.is_file())
    (output / "SHA256SUMS.txt").write_text("".join(
        f"{digest(path)}  {path.relative_to(output).as_posix()}\n" for path in files), encoding="utf-8")
    return output


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--sdk-header", type=Path, required=True)
    args = parser.parse_args()
    try:
        print(package(args.candidate.resolve(), args.output.resolve(), args.sdk_header.resolve()))
    except (OSError, ValueError, KeyError) as error:
        raise SystemExit(f"Packaging refused: {error}") from error
