"""Offline checks of scripts/fetch_tirtc.py: hashes, unpacking and the instructions it prints."""
import hashlib
import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import fetch_tirtc as fetch  # noqa: E402

NAME = "tirtc__fixture_idf6__esp-idf6.0.3-xtensa-esp15.2.0_20251204__v2.6.3__mini"


def archive(folder, top=NAME, library=b"fixture archive"):
    path = Path(folder) / f"{NAME}.tgz"
    with tarfile.open(path, "w:gz") as bundle:
        for name, data in ((f"{top}/lib/libTiRTC.a", library), (f"{top}/include/tirtc/tiRTC.h", b"/* api */")):
            member = tarfile.TarInfo(name)
            member.size = len(data)
            bundle.addfile(member, io.BytesIO(data))
    return path


def fixture_root(folder, package_sha, library=b"fixture archive"):
    root = Path(folder) / "repo"
    sdk = root / "components/tirtc_sdk"
    (sdk / "contracts").mkdir(parents=True)
    (sdk / "contracts/fixture.json").write_text(json.dumps({"sha256": hashlib.sha256(library).hexdigest()}),
                                               encoding="utf-8")
    entry = {"version": "2.6.3", "path": f"fixture/{NAME}", "sha256": package_sha, "contract": "contracts/fixture.json"}
    (sdk / "packages.json").write_text(json.dumps({"release": "http://release.invalid", "default_line": "6.0",
        "lines": {"6.0": {"packages": {"generic-fixture": entry}}}}), encoding="utf-8")
    return root, entry


class FetchTiRTC(unittest.TestCase):
    def test_catalogue_names_every_official_package_by_hash(self):
        data = fetch.catalogue()
        for line in data["lines"].values():
            for entry in line["packages"].values():
                if fetch.fetched(entry):
                    self.assertRegex(entry["sha256"], r"^[0-9a-f]{64}$")
                    self.assertTrue(fetch.url(entry).startswith(data["release"] + "/"))
                    self.assertTrue((ROOT / "components/tirtc_sdk" / entry["contract"]).is_file())

    def test_local_archive_is_checked_unpacked_and_reused(self):
        with tempfile.TemporaryDirectory() as folder:
            tgz = archive(folder)
            root, entry = fixture_root(folder, hashlib.sha256(tgz.read_bytes()).hexdigest())
            where = fetch.ensure(entry, root, sources=[folder], download=False)
            self.assertEqual(where, root / "components/tirtc_sdk/packages" / NAME)
            self.assertEqual((where / "lib/libTiRTC.a").read_bytes(), b"fixture archive")
            tgz.unlink()
            self.assertEqual(fetch.ensure(entry, root, download=False), where)   # already unpacked and valid

    def test_wrong_hash_or_layout_is_refused_with_instructions(self):
        with tempfile.TemporaryDirectory() as folder:
            archive(folder)
            root, entry = fixture_root(folder, "0" * 64)
            with self.assertRaises(fetch.FetchError) as caught:
                fetch.ensure(entry, root, sources=[folder], download=False)
            message = str(caught.exception)
            self.assertIn("http://release.invalid/fixture/", message)
            self.assertIn("0" * 64, message)
            self.assertIn("--from", message)
        with tempfile.TemporaryDirectory() as folder:
            tgz = archive(folder, top="elsewhere")
            root, entry = fixture_root(folder, hashlib.sha256(tgz.read_bytes()).hexdigest())
            with self.assertRaises(fetch.FetchError):
                fetch.ensure(entry, root, sources=[folder], download=False)
            self.assertFalse((root / "components/tirtc_sdk/packages" / NAME).exists())


if __name__ == "__main__":
    unittest.main()
