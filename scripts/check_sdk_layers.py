#!/usr/bin/env python3
# Replaces legacy: tests/test_hardware_boundaries.py
"""SDK layer rules (docs/SDK架构.md section 2 and 6.7).

1. Platform headers. Code under sdk/, apps/ and boards/ includes no platform
   header (ESP-IDF, RTOS, drivers, sdkconfig, pthread, sockets, the TLS
   library): it reaches the platform only through xiaotai/port/*.h and the
   part interfaces. Exempt, in any component under sdk/ and apps/:

     esp-idf/, host/   the component's implementation for one platform; the
                       build compiles only the one that matches the platform
     test/             host tests, which may drive the host directly

   A board package (boards/<vendor>/<board>) has no platform directory: pins
   are numbers, and every platform call belongs to a part of the parts library
   (sdk/parts/<part>/esp-idf). Only its test/ directory is exempt.

2. Third-party libraries. A library header (LVGL, the RTC stack, cJSON) is
   included only by a component whose CMakeLists.txt names that library.

3. Peripherals. Calls that create a bus, a channel or a codec device
   (i2c_new_master_bus, i2s_new_channel, esp_codec_dev_*, esp_lcd_new_panel_*,
   spi_bus_initialize, ...) appear only in sdk/parts/*/esp-idf/ and
   sdk/xiaotai_port/esp-idf/: an engine's esp-idf/ directory uses parts, it
   never creates hardware.

4. Direction. Every component has a layer, from its path:

     port, library   sdk/xiaotai_port, sdk/xiaotai_json
     hal             sdk/xiaotai_hal
     part            sdk/parts/*
     board           boards/<vendor>/<board>
     framework       sdk/xiaotai_framework
     sdk             sdk/xiaotai_sdk (public API)
     engine          every other sdk/xiaotai_* (engines and services)
     theme           sdk/themes/*
     module          sdk/modules/*
     app             apps/*

   An #include "xiaotai/..." and every component a CMakeLists.txt names
   (REQUIRES, xiaotai_host_require, link lists, conditional additions) must
   target a layer the component may use (ALLOWED below). Edges between
   engines, themes, modules and applications must also be declared in the
   manifest (depends, optional_depends; modules and optional_modules of an
   application). The manifest's depends must in turn be required by the
   CMakeLists.txt (scripts/sdk_selection.py requires_mismatch).

5. Hardware needs. sdk/xiaotai_hal/needs.json, xiaotai/needs.h and the table
   of scripts/sdk_selection.py name the same needs with the same bits, and
   every C enumerator of the needs.json vocabulary exists in xiaotai/hal.h.

6. Portable code (sdk/ and apps/ outside esp-idf/, host/ and test/). On
   ESP-IDF xiaotai/config.h brings sdkconfig.h and xiaotai/port/os.h brings
   the RTOS into every includer, so the include rule alone cannot see them:
     - the code (comments and strings aside) reads no CONFIG_* symbol except
       in a configuration header (*_config.h, xiaotai/config.h), and no
       ESP_PLATFORM, IDF_TARGET* or SOC_* macro;
     - the code calls no RTOS, ESP-IDF or driver function directly
       (xTaskCreate, vTaskDelay, heap_caps_*, esp_*, nvs_*, gpio_*, ...);
     - neither code nor comments name a board, a chip or a product: chip
       names (esp32*, and s3, p4, c3, c6, c61 as words), the board families
       and vendors of the board packages, and the names of the boards/
       directories. Differences come from port implementations, part
       descriptors and configuration only. ALLOWED_NAMES lists the few
       documented examples.

7. Local-only intents. An intent whose handler (or a function of the same
   file it calls) erases the identity (xiaotai_identity_erase), forgets
   network credentials (xiaotai_net_forget_*, xiaotai_net_reprovision with
   anything but false) or starts provisioning over (XIAOTAI_PROVISIONING_FORGET)
   is registered with xiaotai_intent_handle_ex(..., XIAOTAI_INTENT_LOCAL), so
   no open channel (AT+INTENT) can post it.

8. Names. The first word of every event, intent and setting a component's
   headers publish (XIAOTAI_EVENT_*, XIAOTAI_INTENT_*, XIAOTAI_SETTING_*) is
   the component's manifest name or a domain DOMAINS registers to it; each
   domain has exactly one owner. Event-log sources and tuning keys always
   use the manifest name (not checked here: they are not macros).
"""
from pathlib import Path
import json
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
# Tree -> directory names exempt in it, and where its platform code belongs.
TREES = {
    "sdk": ({"esp-idf", "host", "test"}, "move the code into the component's esp-idf/ or host/"),
    "apps": ({"esp-idf", "host", "test"}, "move the code into the component's esp-idf/ or host/"),
    "boards": ({"test"}, "a board package holds no platform code: move it into a part under sdk/parts/"),
}
SOURCES = {".c", ".cc", ".cpp", ".h", ".hpp", ".inc"}
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)
PLATFORM = re.compile(
    r"^(esp_|freertos/|driver/|hal/|soc/|rom/|nvs|lwip/|mqtt_client|mbedtls/|psa/|sdkconfig\.h$|"
    r"FreeRTOS\.h$|task\.h$|queue\.h$|semphr\.h$|pthread\.h$|unistd\.h$|sys/socket\.h$|netdb\.h$|"
    r"arpa/|netinet/|sys/select\.h$|fcntl\.h$|termios\.h$)")
# Library header -> the CMake name a component must mention to include it.
LIBRARIES = (
    (re.compile(r"^(lvgl\.h$|lvgl/|lv_)"), "lvgl"),
    (re.compile(r"^(tirtc|TiRTC)"), "tirtc_sdk"),
    (re.compile(r"^cJSON"), "cjson"),
)
PERIPHERAL = re.compile(
    r"\b(i2c_new_master_bus|i2c_master_bus_add_device|i2c_driver_install|i2c_param_config|i2s_new_channel|"
    r"esp_codec_dev_\w+|esp_lcd_new_\w+|spi_bus_initialize|spi_bus_add_device|uart_driver_install|"
    r"ledc_timer_config|ledc_channel_config|rmt_new_\w+|esp_cam_new_\w+|gpio_config|gpio_install_isr_service|"
    r"sdmmc_host_init|usb_serial_jtag_driver_install)\s*\(")
PERIPHERAL_HOMES = ("sdk/parts/", "sdk/xiaotai_port/esp-idf/")

# Rule 6: what portable code may not read, call or name.
PLATFORM_MACRO = re.compile(r"\b(CONFIG_[A-Z0-9_]+|ESP_PLATFORM|IDF_TARGET\w*|SOC_[A-Z0-9_]+)\b")
PLATFORM_CALL = re.compile(
    r"\b((?:x|v|ux|pc|ul)(?:Task|Queue|Semaphore|EventGroup|Timer|StreamBuffer|MessageBuffer|PortGet)\w*"
    r"|portMAX_DELAY|pdMS_TO_TICKS|portTICK_PERIOD_MS|heap_caps_\w+|esp_\w+|ESP_LOG\w|ESP_ERROR_CHECK"
    r"|nvs_\w+|gpio_\w+)\b")
# Chip names, board families and vendors. The boards/ directory names are
# added from the tree.
NAMED = ("esp32[a-z0-9]*", "esp8266", "s3", "p4", "c3", "c6", "c61", "lckfb", "szpi", "m5stack", "cores3",
         "waveshare", "wt99[a-z0-9]*", "xmini", "mz28[a-z0-9-]*")
# (path, name) pairs that are documented examples, not dependencies.
ALLOWED_NAMES = {("sdk/xiaotai_port/include/xiaotai/port/sys.h", "esp32s3")}
CONFIG_HEADER = re.compile(r"(^|_)config\.h$")

# Rule 7: what a handler that anyone may post must not do.
INTENT_REGISTER = re.compile(r"\bxiaotai_intent_handle(_ex)?\s*\(")
FORGETS = re.compile(r"\bxiaotai_identity_erase\s*\(|\bxiaotai_net_forget_\w+\s*\(|"
                     r"\bxiaotai_net_reprovision\s*\(\s*(?!false\b)|\bXIAOTAI_PROVISIONING_FORGET\b")
# Rule 8: name domains other than a unit's manifest name, and their owner (a
# manifest name; "framework" for the runtime framework). Kept in step with
# docs/SDK架构.md section 1.
DOMAINS = {
    "system": "framework", "activity": "framework", "app": "framework", "setting": "framework",
    "view": "framework",
    "talk": "ai_talk",            # intents: frozen bench and voice names
    "vision": "ai_vision",
    "audio": "settings",          # shared settings and their intents: one owner for every product
    "screen": "settings",
    "guidance": "settings",
    "input": "interaction",       # gestures as events, whatever part produced them
    "terminal": "voice_terminal",
}
# Published names that break rule 8 until their owner renames them: name -> owner.
# Empty since wave 1 (stream.camera became stream_video.camera, INDEX 5.3).
PENDING_NAMES = {}
PUBLISHED_NAME = re.compile(r'#\s*define\s+XIAOTAI_(?:EVENT|INTENT|SETTING)_\w+\s+"([a-z][a-z0-9_]*)\.[a-z0-9_.]+"')
CALL = re.compile(r"\b([A-Za-z_]\w*)\s*\(")
KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "do", "else"}

ALLOWED = {
    "port": {"port"},
    "library": {"port", "library"},
    "hal": {"port", "library", "hal"},
    "part": {"port", "library", "hal", "part"},
    "board": {"port", "library", "hal", "part", "board"},
    "framework": {"port", "library", "framework"},
    "sdk": {"port", "library", "hal", "framework", "sdk"},
    "engine": {"port", "library", "hal", "framework", "engine"},
    "theme": {"port", "library", "hal", "framework", "engine", "theme"},
    "module": {"port", "library", "hal", "framework", "engine", "module"},
    "app": {"port", "library", "hal", "framework", "engine", "module"},
}
# Layers whose mutual edges need a manifest declaration.
DECLARED = {"engine", "theme", "module", "app"}
MANIFESTS = ("engine.json", "theme.json", "module.json", "application.json")
# The hardware abstraction links the selected board package (the board
# registers itself through a function the abstraction calls); it includes
# nothing of it.
LINK_ONLY = {("hal", "xiaotai_board")}


class Component:
    def __init__(self, name, path, layer):
        self.name, self.path, self.layer = name, path, layer
        self.manifest_name = None
        self.declared = set()   # manifest names this component may use at its own level


def layer_of(relative):
    parts = relative.parts
    if parts[0] == "boards":
        return "board"
    if parts[0] == "apps":
        return "app"
    group = parts[1]
    if group == "parts":
        return "part"
    if group == "themes":
        return "theme"
    if group == "modules":
        return "module"
    return {"xiaotai_port": "port", "xiaotai_json": "library", "xiaotai_hal": "hal",
            "xiaotai_framework": "framework", "xiaotai_sdk": "sdk"}.get(group, "engine")


def components(root):
    """Component directories with their layer, manifest name and declarations."""
    root = Path(root)
    found = []
    candidates = []
    for pattern in ("sdk/xiaotai_*", "sdk/parts/*", "sdk/themes/*", "sdk/modules/*", "apps/*", "boards/*/*"):
        candidates += [path for path in sorted(root.glob(pattern)) if path.is_dir()]
    for path in candidates:
        relative = path.relative_to(root)
        name = "xiaotai_board" if relative.parts[0] == "boards" else path.name
        component = Component(name, path, layer_of(relative))
        for manifest in MANIFESTS:
            if (path / manifest).is_file():
                data = json.loads((path / manifest).read_text(encoding="utf-8"))
                component.manifest_name = data.get("name")
                for key in ("depends", "optional_depends", "modules", "optional_modules"):
                    component.declared.update(data.get(key, []))
        found.append(component)
    return found


def owner_of(path, components_by_path):
    for parent in [path] + list(path.parents):
        component = components_by_path.get(parent)
        if component:
            return component
    return None


def header_index(found):
    """'xiaotai/media.h' -> component, for every public and platform include dir."""
    index = {}
    for component in found:
        for include in [component.path / "include"] + [component.path / d / "include" for d in ("esp-idf", "host")]:
            if include.is_dir():
                for header in include.rglob("*"):
                    if header.suffix in (".h", ".hpp") and header.is_file():
                        index.setdefault(header.relative_to(include).as_posix(), component)
    return index


def edge_problem(user, target):
    """Why `user` may not use `target`, or None."""
    if target is user:
        return None
    if target.layer not in ALLOWED[user.layer]:
        return f"layer {user.layer} may not use layer {target.layer} ({target.name})"
    if user.layer in DECLARED and target.layer in DECLARED:
        if target.manifest_name and target.manifest_name not in user.declared:
            return f"uses {target.manifest_name} ({target.name}) without declaring it in its manifest"
    return None


def strip_cmake_comments(text):
    return "\n".join(line.split("#", 1)[0] for line in text.splitlines())


def problems(root=ROOT):
    root = Path(root)
    found = []
    comps = components(root)
    by_path = {component.path: component for component in comps}
    by_name = {}
    for component in comps:
        by_name.setdefault(component.name, component)
    index = header_index(comps)
    cmake_text = {}
    for component in comps:
        cmake = component.path / "CMakeLists.txt"
        cmake_text[component.path] = strip_cmake_comments(cmake.read_text(encoding="utf-8")) if cmake.is_file() else ""

    for tree, (exempt, advice) in TREES.items():
        if not (root / tree).is_dir():
            continue
        for path in sorted((root / tree).rglob("*")):
            if path.suffix not in SOURCES or not path.is_file():
                continue
            relative = path.relative_to(root)
            where = relative.as_posix()
            text = path.read_text(encoding="utf-8-sig")
            headers = INCLUDE.findall(text)
            user = owner_of(path.parent, by_path)
            platform_exempt = bool(exempt.intersection(relative.parts[:-1]))
            for header in headers:
                if not platform_exempt and PLATFORM.match(header):
                    found.append(f"{where}: includes platform header {header}; use xiaotai/port/*.h, or {advice}")
                for pattern, library in LIBRARIES:
                    if pattern.match(header) and user and library not in cmake_text.get(user.path, ""):
                        found.append(f"{where}: includes {header} of the library {library}, which its "
                                     "CMakeLists.txt does not name")
                target = index.get(header) if header.startswith("xiaotai") else None
                if user and target:
                    problem = edge_problem(user, target)
                    if problem:
                        found.append(f"{where}: includes {header}: {problem}")
            if PERIPHERAL.search(text) and not (where.startswith(PERIPHERAL_HOMES) and "/esp-idf/" in f"/{where}"
                                                or "test" in relative.parts[:-1]):
                call = PERIPHERAL.search(text).group(1)
                found.append(f"{where}: calls {call}(); creating buses, channels and codec devices belongs to a "
                             "part (sdk/parts/<part>/esp-idf) or the port")

    for component in comps:
        text = cmake_text[component.path]
        where = (component.path / "CMakeLists.txt").relative_to(root).as_posix()
        for token in sorted(set(re.findall(r"\bxiaotai_[a-z0-9_]+\b", text))):
            target = by_name.get(token)
            if target is None or target.name == component.name:
                continue
            if (component.layer, token) in LINK_ONLY:
                continue
            problem = edge_problem(component, target)
            if problem:
                found.append(f"{where}: names {token}: {problem}")

    found += portable_problems(root)
    found += name_problems(comps, root)
    found += manifest_problems(root)
    found += needs_problems(root)
    return found


def strip_c(text, keep_comments=False):
    """The text with string and character literals emptied and, unless kept,
    comments blanked; line breaks stay, so line numbers still match."""
    out, i, n = [], 0, len(text)
    while i < n:
        two = text[i:i + 2]
        if two in ("/*", "//"):
            end = text.find("*/", i + 2) + 2 if two == "/*" else text.find("\n", i)
            end = n if end in (-1, 1) else end
            chunk = text[i:end]
            out.append(chunk if keep_comments else re.sub(r"[^\n]", " ", chunk))
            i = end
        elif text[i] in "\"'":
            quote, j = text[i], i + 1
            while j < n and text[j] != quote and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(quote + quote)
            i = j + 1
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def _names(root):
    names = list(NAMED)
    boards = Path(root) / "boards"
    if boards.is_dir():
        for path in sorted(boards.glob("*/*")):
            if path.is_dir():
                names += [re.escape(path.parent.name.lower()), re.escape(path.name.lower())]
    return re.compile(r"(?<![A-Za-z0-9_])(" + "|".join(dict.fromkeys(names)) + r")(?![A-Za-z0-9_])", re.I)


def _function_bodies(code):
    """name -> body text of every function defined in this (stripped) code."""
    bodies = {}
    for match in re.finditer(r"\b([A-Za-z_]\w*)\s*\([^;{}()]*(?:\([^()]*\)[^;{}()]*)*\)\s*\{", code):
        depth, i = 1, match.end()
        while i < len(code) and depth:
            depth += {"{": 1, "}": -1}.get(code[i], 0)
            i += 1
        if match.group(1) not in KEYWORDS:
            bodies.setdefault(match.group(1), code[match.end():i])
    return bodies


def _arguments(code, start):
    """The top-level arguments of the call whose '(' ends just before start."""
    arguments, depth, current = [], 1, []
    for char in code[start:]:
        if char in "([{":
            depth += 1
        elif char in ")]}":
            depth -= 1
            if depth == 0:
                break
        if char == "," and depth == 1:
            arguments.append("".join(current).strip())
            current = []
        else:
            current.append(char)
    arguments.append("".join(current).strip())
    return arguments


def _forgets(name, bodies, seen):
    """True when the function, or a function of the same file it calls, forgets."""
    if name in seen or name not in bodies:
        return False
    seen.add(name)
    body = bodies[name]
    return bool(FORGETS.search(body)) or any(_forgets(callee, bodies, seen) for callee in CALL.findall(body))


def name_problems(comps, root):
    found = []
    for component in comps:
        owner = "framework" if component.layer == "framework" else component.manifest_name
        include = component.path / "include"
        if not owner or not include.is_dir():
            continue
        for header in sorted(include.rglob("*.h")):
            text = header.read_text(encoding="utf-8")
            for match in PUBLISHED_NAME.finditer(text):
                domain = match.group(1)
                name = match.group(0).split('"')[1]
                if domain == owner or DOMAINS.get(domain) == owner or PENDING_NAMES.get(name) == owner:
                    continue
                number = text.count("\n", 0, match.start()) + 1
                holder = f"owned by {DOMAINS[domain]}" if domain in DOMAINS else "not a registered domain"
                found.append(f"{header.relative_to(root).as_posix()}:{number}: publishes \"{name}\": its first "
                             f"word is not {owner} ({domain} is {holder}; see DOMAINS)")
    return found


def portable_problems(root):
    root = Path(root)
    named = _names(root)
    found = []
    for tree in ("sdk", "apps"):
        if not (root / tree).is_dir():
            continue
        for path in sorted((root / tree).rglob("*")):
            if path.suffix not in SOURCES or not path.is_file():
                continue
            relative = path.relative_to(root)
            where = relative.as_posix()
            if {"esp-idf", "host", "test"}.intersection(relative.parts[:-1]):
                continue
            text = path.read_text(encoding="utf-8-sig")
            code = strip_c(text)
            for number, line in enumerate(code.splitlines(), 1):
                macro = PLATFORM_MACRO.search(line)
                if macro and not (macro.group(1).startswith("CONFIG_") and CONFIG_HEADER.search(path.name)):
                    found.append(f"{where}:{number}: reads {macro.group(1)}; portable code uses its component's "
                                 "<name>_config.h, never the platform's configuration or target macros")
                call = PLATFORM_CALL.search(line)
                if call:
                    found.append(f"{where}:{number}: uses {call.group(1)}; portable code calls the port "
                                 "(xiaotai/port/*.h), never the RTOS or ESP-IDF directly")
            for number, line in enumerate(text.splitlines(), 1):
                for match in named.finditer(line):
                    if (where, match.group(1).lower()) not in ALLOWED_NAMES:
                        found.append(f"{where}:{number}: names {match.group(1)}; sdk/ and apps/ name no board, "
                                     "chip or product (descriptors, ports and configuration carry the difference)")
            if path.suffix in (".c", ".cc", ".cpp"):
                bodies = _function_bodies(code)
                for register in INTENT_REGISTER.finditer(code):
                    arguments = _arguments(code, register.end())
                    local = register.group(1) and len(arguments) >= 4 and "XIAOTAI_INTENT_LOCAL" in arguments[3]
                    if len(arguments) >= 2 and not local and _forgets(arguments[1], bodies, set()):
                        number = code.count("\n", 0, register.start()) + 1
                        found.append(f"{where}:{number}: intent handler {arguments[1]} forgets the identity or "
                                     "network credentials; register it with xiaotai_intent_handle_ex(..., "
                                     "XIAOTAI_INTENT_LOCAL)")
    return found


def manifest_problems(root):
    """Manifest depends that the CMakeLists.txt does not require."""
    script = Path(root) / "scripts/sdk_selection.py"
    if not script.is_file():
        return []
    sys.path.insert(0, str(script.parent))
    try:
        import sdk_selection
        return [f"{path}: {problem}" for path, problem in sdk_selection.requires_mismatch(root)]
    except sdk_selection.SelectionError as error:
        return [f"manifests: {error}"]
    finally:
        sys.path.pop(0)


def needs_problems(root):
    root = Path(root)
    table = root / "sdk/xiaotai_hal/needs.json"
    header = root / "sdk/xiaotai_port/include/xiaotai/needs.h"
    hal = root / "sdk/xiaotai_hal/include/xiaotai/hal.h"
    if not (table.is_file() and header.is_file()):
        return []
    data = json.loads(table.read_text(encoding="utf-8"))
    listed = {entry["name"]: entry["bit"] for entry in data.get("needs", [])}
    declared = {name.lower(): int(bit) for name, bit in
                re.findall(r"XIAOTAI_NEED_([A-Z_]+)\s*=\s*1u\s*<<\s*([0-9]+)", header.read_text(encoding="utf-8"))}
    found = []
    for name in sorted(set(listed) | set(declared)):
        if listed.get(name) != declared.get(name):
            found.append(f"sdk/xiaotai_hal/needs.json: need {name} has bit {listed.get(name)}, "
                         f"xiaotai/needs.h has {declared.get(name)}")
    script = root / "scripts/sdk_selection.py"
    if script.is_file():
        sys.path.insert(0, str(script.parent))
        try:
            import sdk_selection
            table_bits = {name: bit.bit_length() - 1 for name, bit in sdk_selection.NEEDS.items()}
            if table_bits != listed:
                found.append("scripts/sdk_selection.py: NEEDS differs from sdk/xiaotai_hal/needs.json")
        finally:
            sys.path.pop(0)
    if hal.is_file():
        # A vocabulary value must be an enumerator (or macro) defined in hal.h
        # or a header it includes (audio encodings are XIAOTAI_CODEC_* of
        # xiaotai/codec.h); a name that only appears in a comment does not count.
        defined = _defined_names(hal, [root / "sdk/xiaotai_hal/include", root / "sdk/xiaotai_port/include"])
        for spellings in data.get("vocabulary", {}).values():
            for enumerator in spellings.values():
                if enumerator not in defined:
                    found.append(f"sdk/xiaotai_hal/needs.json: {enumerator} is not defined by xiaotai/hal.h "
                                 "or the headers it includes")
    derivation = root / "sdk/xiaotai_hal/src/hal.cc"
    if derivation.is_file():
        found += _derivation_problems(data, derivation.read_text(encoding="utf-8"))
    return found


C_COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)


def _defined_names(header, include_dirs):
    """Enumerators and macros defined by header and the xiaotai/ headers it
    includes, transitively; comments are ignored."""
    names, seen, pending = set(), set(), [header]
    while pending:
        path = pending.pop()
        if path in seen or not path.is_file():
            continue
        seen.add(path)
        text = C_COMMENT.sub(" ", path.read_text(encoding="utf-8"))
        names.update(re.findall(r"^\s*#\s*define\s+([A-Za-z_]\w*)", text, re.M))
        for body in re.findall(r"\benum\b[^{;]*\{(.*?)\}", text, re.S):
            for item in body.split(","):
                match = re.match(r"\s*([A-Za-z_]\w*)", item)
                if match:
                    names.add(match.group(1))
        for included in re.findall(r'^\s*#\s*include\s*[<"](xiaotai/[\w/.]+\.h)[>"]', text, re.M):
            pending += [directory / included for directory in include_dirs]
    return names


def _derivation_problems(data, source):
    """The kind and role tables of src/hal.cc (kInputNeeds, kOutputNeeds,
    kOutputRoleNeeds) against the needs.json rules they implement: every rule
    of the input or output class whose 'where' is a kind list (outputs: plus
    a role family) has exactly its rows, and no row exists without a rule.
    The other needs (audio, display, camera, network, memory) are derived by
    hand-written code that the hal_board test covers."""
    vocabulary = data.get("vocabulary", {})
    source = C_COMMENT.sub(" ", source)

    def rows(table, fields):
        match = re.search(rf"\b{table}\[\]\s*=\s*\{{(.*?)\}};", source, re.S)
        if not match:
            return None
        pattern = r"\{\s*" + r"\s*,\s*".join(fields) + r"\s*\}"
        return set(re.findall(pattern, match.group(1)))

    word = r"(XIAOTAI_\w+)"
    actual = {"kInputNeeds": rows("kInputNeeds", [word, word]),
              "kOutputNeeds": rows("kOutputNeeds", [word, word]),
              "kOutputRoleNeeds": rows("kOutputRoleNeeds", [word, r'"([a-z0-9_.]+)"', word])}
    expected = {name: set() for name in actual}
    found = []
    for rule in data.get("needs", []):
        part, where = rule.get("part"), rule.get("where", {})
        need = "XIAOTAI_NEED_" + rule["name"].upper()
        if part not in ("input", "output") or "kind" not in where:
            if "role" in where:
                found.append(f"sdk/xiaotai_hal/needs.json: need {rule['name']} has a role rule on part "
                             f"class {part}; src/hal.cc derives roles only for outputs")
            continue
        if set(where) - {"kind", "role"} or (part == "input" and "role" in where):
            found.append(f"sdk/xiaotai_hal/needs.json: need {rule['name']}: src/hal.cc has no table for "
                         f"a {part} rule on {sorted(where)}")
            continue
        kinds = [vocabulary.get(f"{part}.kind", {}).get(kind, f"<{kind}>") for kind in where["kind"]]
        if "role" in where:
            expected["kOutputRoleNeeds"] |= {(kind, role, need) for kind in kinds for role in where["role"]}
        else:
            expected["kInputNeeds" if part == "input" else "kOutputNeeds"] |= {(kind, need) for kind in kinds}
    for table, rows_found in actual.items():
        if rows_found is None:
            found.append(f"sdk/xiaotai_hal/src/hal.cc: table {table} not found")
            continue
        for row in sorted(expected[table] - rows_found):
            found.append(f"sdk/xiaotai_hal/src/hal.cc: {table} lacks {{{', '.join(row)}}} (needs.json)")
        for row in sorted(rows_found - expected[table]):
            found.append(f"sdk/xiaotai_hal/src/hal.cc: {table} row {{{', '.join(row)}}} has no needs.json rule")
    return found


def main():
    found = problems()
    for line in found:
        print(line)
    print(f"SDK layer rules: {'FAIL' if found else 'PASS'} ({len(found)} problems)")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
