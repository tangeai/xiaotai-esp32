"""Compile and execute the exact production HTTP handler with bounded mocks."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", default=os.environ.get("CC", "cc"))
    parser.add_argument("--idf", type=Path, required=True)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    tests = Path(__file__).resolve().parent
    component = tests.parent / "components" / "demo_platform"
    source = (component / "platform.c").read_text(encoding="utf-8")
    start = source.index("static void process_request(request_t *request)\n{")
    end = source.index("\nstatic bool take_http_request(", start)
    handler = source[start:end]
    fixture = (tests / "platform_http_metadata_fixture.c").read_text(encoding="utf-8")
    assert fixture.count("/* ACTUAL_PROCESS_REQUEST */") == 1
    with tempfile.TemporaryDirectory(prefix="xmini-http-metadata-") as directory:
        output = Path(directory)
        unit = output / "actual_handler.c"
        unit.write_text(fixture.replace("/* ACTUAL_PROCESS_REQUEST */", handler), encoding="utf-8")
        # Preserve the handler's pre-existing signed sizeof comparison warning.
        command = [args.compiler, "-std=gnu11", "-g", "-Wall", "-Wextra", "-Werror", "-UNDEBUG",
                   "-Wno-error=sign-compare",
                   "-I" + str(tests / "platform_stubs"), "-I" + str(component),
                   "-I" + str(component / "include"), "-I" + str(args.idf / "components/json/cJSON"),
                   str(unit), "-o", str(output / "test")]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(output / "test")], check=True)


if __name__ == "__main__":
    main()
