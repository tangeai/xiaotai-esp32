# SPDX-License-Identifier: MIT
"""Which products this source tree carries.

A source release may leave out a product that is not published yet: its
products/<name>/ folder, its board, vendor files and dependency locks are then
absent. Checks and tests that name such a product use these helpers, so they
cover the products that are here and skip the rest; every other product stays
mandatory.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Products a source release may leave out, and their boards.
OPTIONAL = {"xiaotai-xmini-c3-v3": "xmini/c3-v3"}
XMINI = "xiaotai-xmini-c3-v3"
ABSENT = "this source tree does not carry the product"


def has_product(name, root=ROOT):
    return (Path(root) / "products" / name / "product.json").is_file()


def present(names, root=ROOT):
    """The given products that this tree carries, in the given order."""
    return [name for name in names if has_product(name, root)]


def board_count(root=ROOT):
    """Number of product boards a full tree carries, less those of optional products it left out."""
    return 5 - sum(not has_product(name, root) for name in OPTIONAL)


def expected(names, root=ROOT):
    """Products a tree must carry: all of them, except optional ones it left out."""
    return {name for name in names if name not in OPTIONAL or has_product(name, root)}


def expected_boards(boards, root=ROOT):
    """Boards a tree must carry: all of them, except those of optional products it left out."""
    absent = {board for name, board in OPTIONAL.items() if not has_product(name, root)}
    return set(boards) - absent
