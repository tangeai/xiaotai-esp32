#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Download the official TiRTC packages a build needs and check them.

The repository does not carry TiRTC archives. components/tirtc_sdk/packages.json
names each official package by its path on the TiRTC release server and its
SHA-256; this tool downloads it, checks the hash, unpacks it into
components/tirtc_sdk/packages/ and checks the library against the build
contract. scripts/build.py calls it when a package is missing. A line whose
"origin" is not "official" has packages that are not on the release server:
those are installed from a folder with --from and never downloaded.

    python scripts/fetch_tirtc.py                 # packages of the default ESP-IDF line
    python scripts/fetch_tirtc.py --line 5.5      # packages of one line
    python scripts/fetch_tirtc.py --all           # every line
    python scripts/fetch_tirtc.py --from ~/Downloads  # use archives you downloaded yourself
"""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
SDK = ROOT / "components/tirtc_sdk"
PACKAGES = SDK / "packages"


class FetchError(RuntimeError):
    pass


def catalogue(root=ROOT):
    return json.loads((root / "components/tirtc_sdk/packages.json").read_text(encoding="utf-8"))


def package_entry(line, name):
    entry = line["packages"].get(name)
    if entry is None:
        raise FetchError(f"no TiRTC package {name}")
    return entry


def fetched(entry):
    """True for an official package the build downloads; False for one kept in the tree."""
    return "path" in entry


def archive_name(entry):
    return entry["path"].rsplit("/", 1)[-1] + ".tgz"


def package_dir(entry, root=ROOT):
    return root / "components/tirtc_sdk/packages" / entry["path"].rsplit("/", 1)[-1]


def url(entry, root=ROOT):
    return f"{catalogue(root)['release']}/{entry['path']}/{archive_name(entry)}"


def _line_of(entry, root=ROOT):
    """The ESP-IDF line of a catalogue entry, and whether its packages are on the release server."""
    for name, line in catalogue(root)["lines"].items():
        if any(other.get("path") == entry.get("path") for other in line["packages"].values()):
            return name, line.get("origin", "official") == "official"
    return None, True


def _sha256(path):
    with open(path, "rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def _ready(entry, root):
    contract = json.loads((root / "components/tirtc_sdk" / entry["contract"]).read_text(encoding="utf-8"))
    archive = package_dir(entry, root) / "lib/libTiRTC.a"
    return archive.is_file() and _sha256(archive) == contract["sha256"]


def _download(source, destination):
    request = urllib.request.Request(source, headers={"User-Agent": "xiaotai-fetch-tirtc"})
    with urllib.request.urlopen(request, timeout=120) as response, open(destination, "wb") as out:
        shutil.copyfileobj(response, out, 1 << 20)


def _unpack(archive, entry, root):
    target = package_dir(entry, root)
    parent = target.parent
    parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=parent, prefix=".unpack-") as staging:
        with tarfile.open(archive, "r:gz") as bundle:
            top = {member.name.split("/", 1)[0] for member in bundle.getmembers()}
            if top != {target.name}:
                raise FetchError(f"{archive.name}: unexpected top-level entries {sorted(top)}")
            bundle.extractall(staging, filter="data")
        shutil.rmtree(target, ignore_errors=True)
        Path(staging, target.name).rename(target)


def ensure(entry, root=ROOT, sources=(), download=True):
    """Return the unpacked package directory, fetching it if needed."""
    if not fetched(entry):
        return root / "components/tirtc_sdk" / entry["lib"]
    if _ready(entry, root):
        return package_dir(entry, root)
    name = archive_name(entry)
    with tempfile.TemporaryDirectory(prefix="xiaotai-tirtc-") as temporary:
        archive = None
        for folder in sources:
            candidate = Path(folder) / name
            if candidate.is_file() and _sha256(candidate) == entry["sha256"]:
                archive = candidate
                break
        if archive is None and download and _line_of(entry, root)[1]:
            archive = Path(temporary) / name
            try:
                _download(url(entry, root), archive)
            except OSError as error:
                raise FetchError(_instructions(entry, root, f"download failed: {error}")) from error
        if archive is None or _sha256(archive) != entry["sha256"]:
            raise FetchError(_instructions(entry, root, "no archive with the expected SHA-256"))
        _unpack(archive, entry, root)
    if not _ready(entry, root):
        raise FetchError(f"{name}: libTiRTC.a differs from {entry['contract']}")
    return package_dir(entry, root)


def _instructions(entry, root, reason):
    line, published = _line_of(entry, root)
    if not published:
        return (f"TiRTC package {archive_name(entry)}: the packages of ESP-IDF line {line} are not on the TiRTC "
                f"release server yet ({reason}).\n"
                f"  SHA-256:  {entry['sha256']}\n"
                f"  With the archive at hand: python scripts/fetch_tirtc.py --line {line} --from <folder with the .tgz>")
    return (f"TiRTC package {archive_name(entry)}: {reason}.\n"
            f"  Download: {url(entry, root)}\n"
            f"  SHA-256:  {entry['sha256']}\n"
            f"  Then run: python scripts/fetch_tirtc.py --from <folder with the .tgz>")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--line", help="ESP-IDF line (default: the default line)")
    group.add_argument("--all", action="store_true", help="every ESP-IDF line")
    parser.add_argument("--from", dest="sources", action="append", default=[], type=Path,
                        help="folder holding downloaded .tgz archives; repeatable")
    parser.add_argument("--offline", action="store_true", help="never download")
    args = parser.parse_args(argv)
    data = catalogue()
    lines = list(data["lines"]) if args.all else [args.line or data["default_line"]]
    failed = 0
    for line in lines:
        if line not in data["lines"]:
            parser.error(f"unknown ESP-IDF line {line}")
        for name, entry in data["lines"][line]["packages"].items():
            if not fetched(entry):
                continue
            try:
                where = ensure(entry, sources=args.sources, download=not args.offline)
                print(f"ready  {line}/{name}: TiRTC {entry['version']} in {where.relative_to(ROOT)}")
            except FetchError as error:
                failed += 1
                print(f"FAIL   {line}/{name}: {error}", file=sys.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
