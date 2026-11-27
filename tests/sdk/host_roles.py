# SPDX-License-Identifier: MIT
"""Resolve pinned sources for tests that need third-party components."""
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import host_components  # noqa: E402


class MissingRole(Exception):
    pass


def role_path(role):
    identity = host_components.resolve(role)
    override = os.environ.get(identity.cmake_variable)
    if override:
        directory = Path(override).resolve()
        if not directory.is_dir():
            raise MissingRole(f"{identity.cmake_variable}: no directory at {directory}")
        if host_components.component_hash(directory) != identity.component_hash:
            raise host_components.ComponentError(
                f"{identity.cmake_variable}: {directory} does not match the lock for {role}")
        return directory

    target = host_components.prepared_root() / identity.dirname
    if not target.is_dir():
        raise MissingRole(host_components.instructions(identity, "not prepared"))
    try:
        return host_components.path(role)
    except SystemExit as error:
        raise host_components.ComponentError(str(error)) from None


if __name__ == "__main__":
    try:
        print(role_path(sys.argv[1]))
    except MissingRole as error:
        print(error, file=sys.stderr)
        sys.exit(3)
    except (host_components.ComponentError, OSError) as error:
        print(error, file=sys.stderr)
        sys.exit(2)
