# V3 product manifests

Each `products/<name>/product.json` is the V3 definition of a product. It
selects applications, modules, settings and variants. The V3 board package
supplies the hardware facts; the legacy build entry still uses its old inputs.

```sh
python3 tests/test_products.py
python3 tests/test_products.py --require-products
```

The first check compiles configuration against the SDK headers. When a board
manifest is missing, it checks product declarations independently and reports
`UNRESOLVED_BOARD` with the exact path; it generates no build files for that
product. A board manifest with invalid data fails either check. The second
command requires every board package and compiles the full generated product
glue for defaults, variants and optional module removals.

The live selector always requires a real board package. Neither check boots a
product, links an ESP firmware image or exercises hardware. The C3 RAM budgets
are limits, not measurements. The configured service and binding addresses
are development-tree inputs, not release guidance.
