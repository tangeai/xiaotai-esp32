"""Offline checks of host component preparation: hashing, rejected sources and instructions."""
# Replaces legacy: tests/test_fetch_components.py
import io
import os
from pathlib import Path
import stat
import sys
import tarfile
import tempfile
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fetch_components as fc  # noqa: E402
import host_components as hc  # noqa: E402

REAL_PREPARED = hc.prepared_root()


def make_component(folder, version="1.0.0", body="int a;\n"):
    (folder / "src").mkdir(parents=True)
    (folder / "docs").mkdir()
    (folder / "idf_component.yml").write_text(f"version: {version}\nfiles:\n  exclude:\n  - docs/**/*\n")
    (folder / "CMakeLists.txt").write_text("idf_component_register()\n")
    (folder / "src/a.c").write_text(body)
    (folder / "docs/guide.md").write_text("excluded\n")
    (folder / "CHECKSUMS.json").write_text("{}\n")
    return folder


def write_lock(root, product, component, version, digest):
    # Host components read a product's SDK lock; the native profile marks the product.
    profile = root / "profiles/native-firmware" / f"{product}.json"
    profile.parent.mkdir(parents=True, exist_ok=True)
    profile.write_text("{}\n")
    lock = root / "profiles/dependency-locks" / f"{product}-native.lock"
    lock.parent.mkdir(parents=True, exist_ok=True)
    lock.write_text(f"dependencies:\n  {component}:\n    component_hash: {digest}\n    source:\n"
                    f"      registry_url: https://components.espressif.com/\n      type: service\n"
                    f"    version: {version}\n")


def zip_of(folder, archive, extra=()):
    with zipfile.ZipFile(archive, "w") as bundle:
        for path in sorted(folder.rglob("*")):
            if path.is_file():
                bundle.write(path, path.relative_to(folder).as_posix())
        for info, data in extra:
            bundle.writestr(info, data)
    return archive


def expect_exit(call, *needles):
    try:
        call()
    except SystemExit as error:
        message = str(error)
        for needle in needles:
            assert needle in message, (needle, message)
        return message
    raise AssertionError("expected SystemExit")


def hashing(temp):
    component = make_component(temp / "hash")
    digest = hc.component_hash(component)
    (component / "docs/guide.md").write_text("still excluded\n")
    (component / "CHECKSUMS.json").write_text("[]\n")
    (component / "sub").mkdir()
    (component / "sub/.component_hash").write_text("0\n")
    assert hc.component_hash(component) == digest, "excluded files must not change component_hash"
    (component / "src/a.c").write_text("int b;\n")
    assert hc.component_hash(component) != digest
    included = make_component(temp / "include")
    (included / "idf_component.yml").write_text("files:\n  include:\n  - src/**\n")
    try:
        hc.component_hash(included)
        raise AssertionError("files.include must be refused")
    except hc.ComponentError as error:
        assert "files.include" in str(error)
    if hasattr(os, "symlink"):
        linked = make_component(temp / "linked")
        try:
            os.symlink(linked / "src/a.c", linked / "src/b.c")
        except OSError:
            print("SKIP symlink source (cannot create links here)")
        else:
            try:
                hc.component_hash(linked)
                raise AssertionError("symbolic links must be refused")
            except hc.ComponentError as error:
                assert "symbolic link" in str(error)
    print("PASS: component_hash honours manifest excludes and refuses include/links")


def role_conflict(temp):
    root = temp / "conflict"
    write_lock(root, "p1", "test/demo", "1.0.0", "a" * 64)
    write_lock(root, "p2", "test/demo", "1.1.0", "b" * 64)
    try:
        hc.resolve("demo", root=root, roles={"demo": ("test/demo", ("p1", "p2"))})
        raise AssertionError("disagreeing products must fail")
    except hc.ComponentError as error:
        assert "split the role per product" in str(error) and "p1" in str(error) and "p2" in str(error)
    print("PASS: products locking different versions are reported as a role conflict")


def preparation(temp):
    good = make_component(temp / "good")
    digest = hc.component_hash(good)
    root = temp / "repo"
    write_lock(root, "p1", "test/demo", "1.0.0", digest)
    roles = {"demo": ("test/demo", ("p1",))}
    identity = hc.resolve("demo", root=root, roles=roles)
    base = temp / "prepared"
    os.environ["XIAOTAI_HOST_COMPONENTS"] = str(base)
    target = base / identity.dirname
    common = dict(base=base, caches=[], storages=[], managed=None)

    expect_exit(lambda: hc.path("demo", root=root, roles=roles), "tests/fetch_components.py",
                identity.registry_page, digest, str(target), "not prepared")
    print("PASS: missing preparation names the command, registry page, component_hash and target")

    cache = temp / "cache/service_test"
    make_component(cache / identity.dirname, body="int tampered;\n")
    results, _ = fc.prepare([identity], caches=[temp / "cache"], **{k: v for k, v in common.items() if k != "caches"})
    assert results["demo"]["status"] == "missing" and "rejected cache" in results["demo"]["notes"][0]
    print("PASS: a cache entry with the right name but other content is rejected")

    other = make_component(temp / "other/test__demo", version="1.0.1")
    results, _ = fc.prepare([identity], from_dirs=[temp / "other"], **common)
    assert results["demo"]["status"] == "missing" and "rejected --from" in results["demo"]["notes"][0]
    print("PASS: another version of the component is rejected")

    for name, extra in [("slip", [("../evil.txt", b"x")]), ("absolute", [("/abs.txt", b"x")]),
                        ("drive", [("C:/drive.txt", b"x")])]:
        archive = zip_of(good, temp / f"{name}.zip", extra)
        results, general = fc.prepare([identity], archives=[archive], **common)
        assert results["demo"]["status"] == "missing" and "rejected --archive" in general[0], general
    link = zipfile.ZipInfo("src/link.c")
    link.external_attr = (stat.S_IFLNK | 0o777) << 16
    archive = zip_of(good, temp / "link.zip", [(link, "a.c")])
    results, general = fc.prepare([identity], archives=[archive], **common)
    assert results["demo"]["status"] == "missing" and "not a regular file" in general[0], general
    with tarfile.open(temp / "link.tar", "w") as bundle:
        member = tarfile.TarInfo("src/link.c")
        member.type, member.linkname = tarfile.SYMTYPE, "/etc/passwd"
        bundle.addfile(member)
        data = b"x"
        member = tarfile.TarInfo("CMakeLists.txt")
        member.size = len(data)
        bundle.addfile(member, io.BytesIO(data))
    results, general = fc.prepare([identity], archives=[temp / "link.tar"], **common)
    assert results["demo"]["status"] == "missing" and "not a regular file" in general[0], general
    with tarfile.open(temp / "name.tar", "w", format=tarfile.GNU_FORMAT, encoding="utf-8",
                      errors="surrogateescape") as bundle:
        data = b"x"
        member = tarfile.TarInfo("src/\udcff.c")  # raw byte 0xff, not UTF-8
        member.size = len(data)
        bundle.addfile(member, io.BytesIO(data))
    results, general = fc.prepare([identity], archives=[temp / "name.tar"], **common)
    assert results["demo"]["status"] == "missing" and "not valid UTF-8" in general[0], general
    assert not (temp / "evil.txt").exists() and not target.exists()
    print("PASS: zip-slip, absolute, drive-letter, symbolic-link and non-UTF-8 archive members are rejected")

    results, _ = fc.prepare([identity], from_dirs=[temp / "no-such-dir"], **common)
    assert "rejected --from" in results["demo"]["notes"][0] and "not a directory" in results["demo"]["notes"][0]
    text = hc.instructions(identity, "missing", base, " --role demo --idf 5.5")
    assert "tests/fetch_components.py --role demo --idf 5.5 --archive" in text, text
    print("PASS: a missing --from directory is named; hints repeat the user's --role/--idf")

    archive = zip_of(good, temp / "good.zip")
    results, general = fc.prepare([identity], archives=[archive], **common)
    assert results["demo"]["status"] == "ready" and not general, (results, general)
    assert hc.path("demo", root=root, roles=roles) == target
    assert (target / "src/a.c").is_file() and not (target / "CHECKSUMS.json").exists()
    assert not (target / "docs").exists(), "only files covered by component_hash are copied"
    fc.install(identity, good, "concurrent run", base)  # target already present and valid: kept
    assert hc.check_prepared(identity, base) is None
    print("PASS: a verified archive is staged, placed and stamped; a concurrent valid copy is kept")

    entries = fc.write_paths([identity], results, base, roles=roles, root=root)
    assert "XIAOTAI_HOST_DEMO_DIR" in (base / "paths.cmake").read_text() and "demo" in entries
    unreadable = temp / "no-locks"  # resolution fails: the earlier verified entry stays
    assert "demo" in fc.write_paths([], {}, base, roles=roles, root=unreadable)
    assert "XIAOTAI_HOST_DEMO_DIR" in (base / "paths.cmake").read_text()
    before = (base / "paths.json").read_bytes()
    saved = os.environ.pop(hc.LINE_VARIABLE, None)
    try:
        assert fc.main(["--idf", "9.9"]) == 1
    finally:
        os.environ.pop(hc.LINE_VARIABLE, None)
        if saved is not None:
            os.environ[hc.LINE_VARIABLE] = saved
    assert (base / "paths.json").read_bytes() == before, "a bad --idf must not touch paths.*"
    print("PASS: paths.* keep verified entries when resolution fails; a bad --idf stops first")

    with open(target / "src/a.c", "a") as stream:
        stream.write("/* edit */\n")
    expect_exit(lambda: hc.path("demo", root=root, roles=roles), "files changed")
    results, _ = fc.prepare([identity], **common)
    assert results["demo"]["status"] == "missing" and "prepared copy rejected" in results["demo"]["notes"][0]
    results, _ = fc.prepare([identity], from_dirs=[good], **common)
    assert results["demo"]["status"] == "ready" and hc.check_prepared(identity, base) is None
    print("PASS: an edited prepared file is caught at use time and replaced from a verified source")

    info = os.stat(target / "src/a.c")
    os.utime(target / "src/a.c", ns=(info.st_atime_ns, info.st_mtime_ns + 1_000_000_000))
    assert hc.check_prepared(identity, base) == "files changed since preparation"
    results, _ = fc.prepare([identity], **common)
    assert results["demo"]["source"] == "prepared (re-verified)"
    print("PASS: a touched but unchanged prepared copy is re-verified by content")
    del os.environ["XIAOTAI_HOST_COMPONENTS"]


def real_lock_hash():
    identity = hc.resolve("cjson")
    candidates = [REAL_PREPARED / identity.dirname] if hc.check_prepared(identity, REAL_PREPARED) is None else []
    candidates += [path for path, _ in fc.cache_candidates(identity, fc.cache_roots(isolated=True))]
    if not candidates:
        print("SKIP: no prepared or cached espressif/cjson to compare with the lock hash")
        return
    assert hc.component_hash(candidates[0]) == identity.component_hash, candidates[0]
    try:
        from idf_component_tools.hash_tools.calculate import hash_dir
    except ImportError:
        print(f"PASS: component_hash of {candidates[0]} equals the product lock")
        return
    exclude = set((hc.read_manifest(candidates[0]).get("files") or {}).get("exclude") or []) | set(hc.HASH_EXCLUDE)
    assert hash_dir(candidates[0], exclude=exclude, exclude_default=False) == identity.component_hash
    print("PASS: component_hash equals the product lock and idf_component_tools.hash_dir")


def main():
    with tempfile.TemporaryDirectory(prefix="xiaotai-fetch-") as folder:
        temp = Path(folder)
        hashing(temp)
        role_conflict(temp)
        preparation(temp)
    real_lock_hash()


if __name__ == "__main__":
    main()
