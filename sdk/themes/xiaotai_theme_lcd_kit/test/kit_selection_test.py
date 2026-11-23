#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""theme_lcd_kit_selection: theme selection for five products and a headless fixture.

  kit_selection_test.py <tree root>

Runs scripts/sdk_selection.py over the real catalog of the tree the way a
product build does (plan(): resolution, needs, the display check of
check_themes(), the theme parameter check, build options, components, the
generated unit table). The product fixtures follow the shipping SDK products
and the boards give the relevant package parts (display size and pixel
format, touch, audio encoding, camera, network kind, external RAM), which
is all the selection reads. Exits non-zero on the first failed check.
"""
import importlib.util
from pathlib import Path
import sys

KIT = "theme_lcd_kit"
COLOUR_THEMES = ("theme_lcd320", "theme_lcd480")


def load_selection(root):
    spec = importlib.util.spec_from_file_location("sdk_selection", root / "scripts/sdk_selection.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def board(display=None, encoding="pcm16", camera=False, network="wifi", external_ram=True, key=True):
    parts = {"audio_input": [{"encoding": encoding, "rate_hz": 16000}], "audio_output": [{}],
             "network": [{"kind": network, "wifi": {}}]}
    if display is not None:
        parts["display"] = [display]
    if camera:
        parts["camera"] = [{}]
    if key:
        parts["input"] = [{"kind": "button", "role": "main"}]
    return {"parts": parts, "memory": {"external_ram": external_ram}}


S3_MODULES = ["theme_lcd320", "contacts", "ai_talk", "wake", "ai_vision", "call", "wechat", "room", "stream",
              "stream_video", "recognition", "environment"]
P4_MODULES = ["theme_lcd480", "contacts", "ai_talk", "wake", "call", "wechat", "video_call", "room", "stream",
              "stream_video", "environment"]
QVGA = {"pixel": "rgb565", "width": 320, "height": 240, "touch": True}
# The 3.5-inch panel is native 320x480, rotated to a logical 480x320.
HVGA = {"pixel": "rgb565", "width": 480, "height": 320, "touch": True}
WSVGA = {"pixel": "rgb565", "width": 1024, "height": 600, "touch": True}
MONO = {"pixel": "mono1_vlsb", "width": 128, "height": 64}

# name -> (product, board, theme units expected in initialization order)
PRODUCTS = {
    "xiaotai-szpi-s3": (
        {"applications": ["companion"], "modules": S3_MODULES},
        board(QVGA, camera=True), [KIT, "theme_lcd320"]),
    "xiaotai-cores3-lite": (
        {"applications": ["companion"], "modules": [m for m in S3_MODULES if m != "ai_vision"]},
        board(QVGA, camera=True), [KIT, "theme_lcd320"]),
    "xiaotai-waveshare-p4-rev3": (
        {"applications": ["companion"], "modules": P4_MODULES},
        board(HVGA, camera=True, network="wifi_coprocessor"), [KIT, "theme_lcd480"]),
    "xiaotai-wt99p461-s1-1v1": (
        {"applications": ["door", "companion"], "home": "door", "modules": P4_MODULES},
        board(WSVGA, camera=True, network="wifi_coprocessor"), [KIT, "theme_lcd480"]),
    "xiaotai-xmini-c3-v3": (
        {"applications": ["companion"],
         "modules": ["theme_mono128", "contacts", "ai_talk", "call", "wechat", "room", "stream", "environment"]},
        board(MONO, external_ram=False), ["theme_mono128"]),
}
HEADLESS = ({"applications": ["voice_terminal"], "modules": ["settings", "environment"]},
            board(None, encoding="opus", external_ram=False), [])

failures = []


def check(condition, message):
    if not condition:
        failures.append(message)
        print(f"FAIL: {message}", flush=True)


def product(name, definition, board_name="acme/board"):
    return {"schema": 1, "name": name, "board": board_name, **definition}


def refused(selection, root, definition, parts, expected):
    try:
        selection.plan(root, definition, parts)
    except selection.SelectionError as error:
        check(expected in str(error), f"{definition['name']}: refused with {error!s}, expected {expected!r}")
        return
    check(False, f"{definition['name']}: accepted, expected a refusal containing {expected!r}")


def accepted(selection, root, definition, parts):
    try:
        selection.plan(root, definition, parts)
    except selection.SelectionError as error:
        check(False, f"{definition['name']}: refused: {error}")


def main(argv):
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    root = Path(argv[1]).resolve()
    selection = load_selection(root)

    # The manifests: the kit is a theme unit without a display rule; every
    # registered theme stands on its own (no theme pulls another registered
    # theme into a product).
    catalog = selection.catalog(root)["module"]
    kit = catalog[KIT]
    check(kit.kind == "theme" and kit.engine, f"{KIT} is a theme unit initialized with the engines")
    check(kit.display is None, f"{KIT} has no display rule (it registers no theme): {kit.display}")
    check(list(kit.depends) == ["ui"], f"{KIT} depends on ui only: {kit.depends}")
    check(sorted(kit.needs) == ["display"], f"{KIT} needs display: {kit.needs}")
    check(not kit.start, f"{KIT} has nothing to start")
    check(kit.options == {"UI_LVGL": True}, f"{KIT} switches the LVGL backend on: {kit.options}")
    check(kit.component == "xiaotai_theme_lcd_kit", f"{KIT} is component xiaotai_theme_lcd_kit: {kit.component}")
    for name in COLOUR_THEMES:
        unit = catalog[name]
        check(list(unit.depends) == ["ui", KIT], f"{name} depends on ui and the kit: {unit.depends}")
        check(unit.options == {"UI_LVGL": True}, f"{name} options: {unit.options}")
    check(catalog["theme_lcd320"].display == {"pixel": ["rgb565"], "width": [320, 320], "height": [240, 240]},
          f"theme_lcd320 display rule: {catalog['theme_lcd320'].display}")
    check(catalog["theme_lcd480"].display == {"pixel": ["rgb565"], "width": [480, 65535],
                                              "height": [320, 65535]},
          f"theme_lcd480 display rule: {catalog['theme_lcd480'].display}")
    registered = {name for name, unit in catalog.items() if unit.kind == "theme" and unit.display is not None}
    for name in sorted(registered):
        dragged = [dep for dep in catalog[name].depends if dep in registered]
        check(not dragged, f"{name} depends on registered themes {dragged}")

    # The five product fixtures and an independent headless extension.
    for name, (definition, parts, themes) in {**PRODUCTS, "headless-voice-fixture": HEADLESS}.items():
        try:
            chosen = selection.plan(root, product(name, definition), parts)
        except selection.SelectionError as error:
            check(False, f"{name}: refused: {error}")
            continue
        modules = chosen["modules"]
        theme_units = [unit for unit in modules if unit.startswith("theme_")]
        check(theme_units == themes, f"{name}: theme units {theme_units}, expected {themes}")
        if themes:
            check(modules.index("ui") < modules.index(themes[0]), f"{name}: ui not before {themes[0]}: {modules}")
        else:
            check("ui" not in modules, f"{name}: has no display and selects no UI system: {modules}")
        colour = KIT in themes
        check(("CONFIG_XIAOTAI_UI_LVGL=y" in chosen["options"]) == colour,
              f"{name}: LVGL backend {'on' if colour else 'off'} expected: {chosen['options']}")
        check(("xiaotai_theme_lcd_kit" in chosen["components"]) == colour,
              f"{name}: kit component {'built' if colour else 'not built'} expected")
        for theme in COLOUR_THEMES:
            component = "xiaotai_" + theme
            check((component in chosen["components"]) == (theme in themes),
                  f"{name}: component {component} {'built' if theme in themes else 'not built'} expected")
        apps, units, _ = chosen["units"]
        table = selection.generate(apps, units)
        if colour:
            check("xiaotai_err_t xiaotai_engine_theme_lcd_kit_init(void);" in table
                  and ".init = xiaotai_engine_theme_lcd_kit_init," in table,
                  f"{name}: the generated unit table initializes the kit")
            depends = f'module_{themes[1]}_depends[] = {{"ui", "{KIT}", NULL}};'
            check(depends in table, f"{name}: generated dependencies of {themes[1]} are ui and the kit")
        else:
            check(KIT not in table, f"{name}: the generated unit table has no kit")

    # The display check sees through the kit: it is not a theme for any
    # display, and a colour theme on the wrong size is refused before building.
    s3, s3_board, _ = PRODUCTS["xiaotai-szpi-s3"]
    p4, p4_board, _ = PRODUCTS["xiaotai-waveshare-p4-rev3"]
    wt99, wt99_board, _ = PRODUCTS["xiaotai-wt99p461-s1-1v1"]
    without_theme = [m for m in s3["modules"] if m != "theme_lcd320"]
    # The kit alone is never a theme that "draws for none" (on colour, mono
    # or several displays), and never stands in for a theme a display lacks.
    two_displays = {"parts": dict(s3_board["parts"], display=[QVGA, MONO])}
    for parts in (s3_board, p4_board, PRODUCTS["xiaotai-xmini-c3-v3"][1], two_displays):
        try:
            selection.check_themes([kit], parts)
        except selection.SelectionError as error:
            check(False, f"check_themes counts the kit as a theme: {error}")
    for theme, parts, display in (("theme_lcd320", p4_board, HVGA), ("theme_lcd480", s3_board, QVGA)):
        try:
            selection.check_themes([kit, catalog[theme]], parts)
            check(False, f"check_themes accepts {theme} on {display}")
        except selection.SelectionError as error:
            check(f"theme {theme} draws for none" in str(error), f"check_themes: {error}")
    try:
        selection.check_themes([kit, catalog["theme_lcd320"]], two_displays)
        check(False, "check_themes accepts a mono display drawn by no theme beside the kit")
    except selection.SelectionError as error:
        check("no selected theme draws for the board's display" in str(error) and "mono1_vlsb" in str(error),
              f"check_themes: {error}")
    refused(selection, root, product("large-on-qvga", dict(s3, modules=without_theme + ["theme_lcd480"])),
            s3_board, "theme theme_lcd480 draws for none of the board's displays")
    for name, definition, parts in (("qvga-on-hvga", p4, p4_board), ("qvga-on-wsvga", wt99, wt99_board)):
        refused(selection, root, product(name, dict(definition, modules=definition["modules"] + ["theme_lcd320"])),
                parts, "theme theme_lcd320 draws for none of the board's displays")
    # A colour theme on the mono panel.
    xmini, xmini_board, _ = PRODUCTS["xiaotai-xmini-c3-v3"]
    refused(selection, root, product("qvga-on-mono", dict(xmini, modules=["theme_lcd320"] + xmini["modules"][1:])),
            xmini_board, "theme theme_lcd320 draws for none of the board's displays")
    # The theme a product names must be selected and draw for the first
    # display: lcd320 on the 320x240 products, lcd480 on the larger ones.
    accepted(selection, root, product("named-qvga", dict(s3, parameters={"ui": {"theme": "lcd320"}})), s3_board)
    refused(selection, root, product("named-large", dict(s3, parameters={"ui": {"theme": "lcd480"}})), s3_board,
            "selects no theme unit theme_lcd480")
    accepted(selection, root, product("named-large-p4", dict(p4, parameters={"ui": {"theme": "lcd480"}})),
             p4_board)
    refused(selection, root, product("named-qvga-p4", dict(p4, parameters={"ui": {"theme": "lcd320"}})), p4_board,
            "selects no theme unit theme_lcd320")

    # Each theme component requires in CMake what its manifest depends on.
    mismatches = [entry for entry in selection.requires_mismatch(root) if entry[0].startswith("sdk/themes/")]
    check(not mismatches, f"theme CMakeLists.txt and manifests disagree: {mismatches}")

    if failures:
        print(f"FAIL: theme_lcd_kit_selection: {len(failures)} checks failed", flush=True)
        return 1
    print("PASS: theme_lcd_kit_selection: kit manifest, five product and headless selections, display check "
          "refusals, named themes, CMake requirements", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
