#!/usr/bin/env python3
"""Selects what a product builds: applications, the base modules they need and
the engines those need, and generates what xiaotai_start() consumes.

Each engine (sdk/<component>/engine.json), theme (sdk/themes/<component>/theme.json,
handled as an engine), base module (sdk/modules/<component>/module.json) and
application (apps/<component>/application.json) carries one manifest, the only
place its name, dependencies and needs are declared:

  engine:      {"name": "media", "depends": [], "needs": [], "start": true}
  module:      {"name": "ai_talk", "depends": ["media", "transport", "binding"],
                "needs": ["audio_in", "audio_out"], "start": true}
  application: {"name": "companion", "modules": [...], "optional_modules": [...],
                "needs": [...], "start": true, "foreground": true, "page_step": true}

Optional manifest fields:

  "optional_depends": ["call"]      engines and modules: initialized after these when
                                    the product selects them; never pulled in
  "one_of": [["call", "wechat"]]    engines and modules: at least one unit of each
                                    group must be selected by the product
  "configure": [ {...}, ... ]       every kind: product configuration calls, see below
  "options": {"UI_LVGL": true}      every kind: build options (CONFIG_XIAOTAI_<KEY>)
                                    the unit cannot work without; written for every
                                    product that selects it, and a product option that
                                    contradicts one is an error
  "display": {"pixel": ["rgb565"],  themes: the displays the theme draws for (pixel
              "width": [320, 480],  formats of board.json "parts.display", inclusive
              "height": [240, 320]} ranges). A selected theme that matches none of the
                                    board's displays, or a board display that no
                                    selected theme matches, is an error
  "activities": {"call": 2}         engines, modules and applications: the foreground
                                    activities the unit begins and their default
                                    priorities (1..255), generated into
                                    xiaotai_module_t.activities and
                                    xiaotai_application_t.activities. Two selected
                                    units giving one activity different defaults is an
                                    error; the product's "priorities" override them by
                                    name
  "network_kinds": ["ethernet"]     engines and modules: the kinds of network part
                                    (board.json "parts.network[].kind", spellings of
                                    sdk/xiaotai_hal/needs.json "vocabulary") the unit
                                    drives. When a selected unit needs "network", every
                                    kind the board lists must be driven by a selected
                                    unit; a kind exactly one unit of the catalog drives
                                    selects that unit, none or several is an error
  "handles": ["call.incoming"]      applications: the module events the application
                                    presents (xiaotai_application_t.handles); each
                                    name is "<unit>.<what>" of a unit in the catalog
  "claims": true                    applications with "handles": the application
                                    decides per event (xiaotai_app_<name>_claim)
  "page_step": true                 applications that page within their home views;
                                    calls xiaotai_app_<name>_page_step in the foreground
  "registrations": {"intents": 2,   maximum init-time registrations performed by this unit;
    "subscribers": 3, "settings": 1, "commands": 1,
                                    each when_selected entry adds its counts once if any
    "when_selected": [{"any_of":     of its declared optional dependencies is selected
     ["media"], "subscribers": 1}]}  (runtime conditions reserve their maximum)
    "commands" counts framework command roots, not local commands or verbs.
    Count overlapping roots once per owner unit, so subsets stay safe.
    Settings and commands size the default tables on boards without external RAM.
  "commands": [{"command": "AT+X",  every kind: the AT spellings the unit registers
    "verb": "SET", "usage": "...",  (spec xiaotai_at.md 4.0; access read, action or
    "access": "guarded"}]           guarded; "option" and "symbol" together when a
                                    build option compiles the handler). Checked, and
                                    merged over a product's selected units into its
                                    vocabulary (commands.json), which the bench tool's
                                    dialect, the console script's permissions and the
                                    public command list are generated from; the bare
                                    "AT" of the console engine is in every vocabulary

Engines and modules share one lifecycle in the framework: a module depends on
engines and other modules by name and is initialized after them; engines never
depend on modules, so every engine is initialized before every module, and the
applications last. The engines "eventlog", "at" and "identity" are part of
every product and come first, in this order (identity installs the
framework's setting store at its init, before any setting is registered).
An optional sdkconfig.defaults beside a selected manifest supplies platform
defaults, which the product's "sdkconfig" can override.

A product (products/<name>/product.json) names a board package, its
applications, extra modules (optional modules of its applications, themes,
anything else), a home application, activity priorities and preemption rules
("preemptions": [{"activity": "recognition", "over": "room", "preempts":
false}], every name an activity a selected unit declares), and optionally
"needs_all" and "needs_any": respectively all or at least one of the listed
board needs must be present. Both constraints apply when both are given. They
do not select drivers, modules or interaction styles. The product also sets
what happens after a failed start ("start_failure_restart_ms", xiaotai/xiaotai.h), parameters for
the configure calls, build options, the TiRTC package per ESP-IDF line, the
internal RAM it may hold once started ("budget": {"internal_bytes": N,
"baseline_bytes": M}: the limit, checked by the whole-product host test through
XIAOTAI_PRODUCT_INTERNAL_BUDGET_BYTES, and the firmware it replaces measured the
same way, XIAOTAI_PRODUCT_INTERNAL_BASELINE_BYTES, which the test prints the
difference to) and the variants it may be built as (the same product without
some extra modules, with other build options, or with some parameter fields
replaced). Resolution: applications -> their modules (optional ones only when
the product selects them) -> dependencies, transitively -> the drivers of the
board's network kinds. Needs are checked against the board (the "parts" of its
board.json, by the rules of sdk/xiaotai_hal/needs.json, whose "vocabulary"
also gives the spellings the parts' fields may take) before building. A theme
named in the parameters (the field a configure entry overrides from
xiaotai_config_t.theme) must be a selected theme unit, "theme_<name>"
(xiaotai/ui_theme.h), that draws for the board's first display. The generated C file defines each
selected unit from its manifest and points at functions named by convention:
xiaotai_engine_<name>_init, xiaotai_module_<name>_init, xiaotai_app_<name>_init,
..._start, ..._foreground, xiaotai_app_<name>_claim and ..._page_step; for a product it also
defines the product glue xiaotai_selected_configure(), the product's
xiaotai_config_t and the needs the board package declares,
xiaotai_board_declared_needs (all three in xiaotai/selection.h).

Product configuration calls. A "configure" entry of a manifest declares one
function the product glue calls before the framework starts:

  {"function": "xiaotai_call_configure", "header": "xiaotai/modules/call.h",
   "type": "xiaotai_call_config_t"}

  form       "struct" (default): fn(const <type> *); "table": fn(const <type> *, size_t);
             "call": fn(void)
  section    key under the product's "parameters" (default: the manifest name)
  always     call even when the product gives no values (struct: NULL; table: NULL, 0)
  arrays     {"field": {"type": "<element type>", "count": "<count field>"}} or
             {"field": {"type": "...", "terminated": true}}: a JSON list for that
             field becomes a static array (count field set, or NULL-terminated)
  pointers   {"field": "<type>"}: a JSON object for that field becomes a static
             object and the field its address; equal objects are emitted once
  pairs      table form: ["name", "value"] lets the product write {"a": 1, "b": 2}
             for a table of {name, value} elements
  overrides  {"<struct field>": "<xiaotai_config_t member>"}: the run-time
             configuration replaces the product's value when it is not NULL
  absent_with {"<unit>": ["<field>", ...]}: when the product selects that unit,
             these fields of the section are refused (another unit owns them)

Product values are written as designated initializers, so the C compiler
checks every name and type: numbers and booleans as they are, null as NULL, a
string that looks like a constant (XIAOTAI_...) as that identifier, any other
string as a string literal, an object as a nested struct. A string
"@board.<path>" stands for that value of the board package's manifest
("@board.identity.profile_model"), so a board fact is written once.
"""
from dataclasses import dataclass, field
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
NAME = re.compile(r"^[a-z][a-z0-9_]{0,30}$")
PRODUCT_NAME = re.compile(r"^[a-z0-9][a-z0-9-]*$")
BOARD_NAME = re.compile(r"^[a-z0-9][a-z0-9_.-]*/[a-z0-9][a-z0-9_.-]*$")
C_IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
C_CONSTANT = re.compile(r"^XIAOTAI_[A-Z0-9_]+$")
C_TYPE = re.compile(r"^(const )?[A-Za-z_][A-Za-z0-9_]*( \*)?$")
OPTION = re.compile(r"^[A-Z][A-Z0-9_]*$")
REGISTRY_OPTIONS = {"intents": "FRAMEWORK_MAX_INTENTS", "subscribers": "FRAMEWORK_MAX_SUBSCRIBERS"}
SETTINGS_REGISTRY_OPTION = "FRAMEWORK_MAX_SETTINGS"
COMMAND_REGISTRY_OPTION = "FRAMEWORK_MAX_COMMANDS"
TIRTC_IDLE_STACK_COMPACT = "TIRTC_IDLE_STACK_COMPACT"
LARGE_TEXT_OPTION = "THEME_LCD_LARGE_TEXT"
DOOR_TEMPLATES_OPTION = "DOOR_TEMPLATES"
# Match the integer ranges in sdk/xiaotai_framework/Kconfig.
REGISTRY_RANGES = {"intents": (16, 1024), "subscribers": (16, 1024), "settings": (8, 512), "commands": (8, 512)}
# A parameter value "@board.identity.profile_model" is that value of the board package's manifest.
BOARD_REFERENCE = "@board."

NEEDS_TABLE = ROOT / "sdk/xiaotai_hal/needs.json"


def _need_rules(path=NEEDS_TABLE):
    """The hardware needs and the rules that derive them from board.json
    "parts" (sdk/xiaotai_hal/needs.json, the one table that xiaotai/needs.h and
    xiaotai_hal_needs() follow too), and the spellings part fields may take
    ("vocabulary": "<class>.<field>" -> spelling -> C enumerator). Only "needs"
    counts; "requested" entries have no bit yet."""
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    rules = []
    for entry in data.get("needs", []):
        if (not isinstance(entry, dict) or not NAME.match(str(entry.get("name", "")))
                or not isinstance(entry.get("bit"), int) or ("part" in entry) == ("memory" in entry)
                or not isinstance(entry.get("where", {}), dict)):
            raise RuntimeError(f"{path}: bad need {entry!r}")
        rules.append(entry)
    vocabulary = data.get("vocabulary", {})
    if not isinstance(vocabulary, dict) or not all(isinstance(words, dict) for words in vocabulary.values()):
        raise RuntimeError(f"{path}: vocabulary maps \"<class>.<field>\" to spellings")
    return data.get("defaults", {}), rules, vocabulary


NEED_DEFAULTS, NEED_RULES, PART_VOCABULARY = _need_rules()
# A "role" entry of a rule's "where" names a role family (needs.json): "door"
# matches the role "door" and any role continuing with ".", such as "door.gate".
ROLE_FIELD = "role"
# Need names (xiaotai/needs.h: XIAOTAI_NEED_<NAME>) and their bits.
NEEDS = {rule["name"]: 1 << rule["bit"] for rule in NEED_RULES}
# Capabilities of schema-2 board manifests (the --capabilities entry only).
CAPABILITY_NEEDS = {
    "audio.pcm.capture": {"audio_in", "pcm_in"},
    "audio.opus16k20.capture": {"audio_in"},
    "audio.pcm.playback": {"audio_out"},
    "display.rgb565": {"display"},
    "display.mono1": {"display"},
    "input.pointer": {"touch"},
    "input.key": {"key"},
    "feedback.rgb": {"light"},
    "output.relay": {"relay"},
    "camera.capture": {"camera"},
    "network.ip": {"network"},
    "memory.psram": {"external_ram"},
}
PATTERNS = (("engine", "sdk/*/engine.json"), ("theme", "sdk/themes/*/theme.json"),
            ("module", "sdk/modules/*/module.json"), ("application", "apps/*/application.json"))
# Part of every product, initialized before every other engine, in this order
# (xiaotai/selection.h): the event log, the console, then identity, whose
# preferences service becomes the framework's setting store at its init.
BASE_ENGINES = ("eventlog", "at", "identity")
# Components every product links whatever it selects (xiaotai_json: the
# common JSON reader, a library without a manifest, D40).
BASE_COMPONENTS = ("xiaotai_port", "xiaotai_json", "xiaotai_framework", "xiaotai_hal", "xiaotai_sdk")
CONFIGURE_FORMS = ("struct", "table", "call")
CONFIGURE_FIELDS = {"function", "header", "type", "form", "section", "always", "arrays", "pointers", "pairs",
                    "overrides", "absent_with"}
# xiaotai_config_t members a configure entry may name in "overrides".
CONFIG_MEMBERS = ("theme", "discovery_url")
PRODUCT_FIELDS = {"schema", "name", "board", "applications", "home", "modules", "priorities", "preemptions",
                  "needs_all", "needs_any",
                  "start_failure_restart_ms", "local_on_storage_boot_failure", "parameters", "options", "tirtc",
                  "sdkconfig", "variants", "budget"}
VARIANT_FIELDS = {"without", "options", "parameters"}
BUDGET_FIELDS = {"internal_bytes", "baseline_bytes"}
DISPLAY_FIELDS = {"pixel", "width", "height"}
PREEMPTION_FIELDS = {"activity", "over", "preempts"}
# xiaotai_config_t.start_failure_restart_ms: XIAOTAI_START_HALT (xiaotai/xiaotai.h).
START_HALT = -1
# A module event name: "<unit>.<what>" (xiaotai/framework.h).
EVENT_NAME = re.compile(r"^([a-z][a-z0-9_]*)\.[a-z0-9_.]+$")
# A public header's product configuration function: xiaotai_<unit>_configure(...).
CONFIGURE_DECLARATION = re.compile(r"^xiaotai_err_t\s+(xiaotai_[a-z0-9_]+_configure)\s*\(", re.M)
# Manifest "commands" (spec xiaotai_at.md 4.0).
COMMAND_FIELDS = {"command", "verb", "usage", "access", "option", "symbol"}
COMMAND_ACCESS = ("read", "action", "guarded")
AT_COMMAND = re.compile(r"^AT\+[A-Z0-9_]+$")
AT_LOCAL_QUERY = re.compile(r"AT\+[A-Z0-9_]+\?")
AT_VERB = re.compile(r"^[A-Z0-9_]+$")
# The console engine and the request it answers without a name (xiaotai_at.md 4.1).
CONSOLE_ENGINE = "at"
BARE_AT = "AT"
# A public header's activity name: #define XIAOTAI_<UNIT>_ACTIVITY "call".
ACTIVITY_DECLARATION = re.compile(r'^#define\s+XIAOTAI_[A-Z0-9_]*_ACTIVITY\s+"([a-z][a-z0-9_]*)"', re.M)
# The xiaotai_config_t member that names a theme; a theme unit registers its
# theme under its manifest name without this prefix (xiaotai/ui_theme.h).
THEME_MEMBER = "theme"
THEME_PREFIX = "theme_"
# board.json part class whose entries the network drivers serve, and the need
# of a unit that uses the network (sdk/xiaotai_hal/needs.json).
NETWORK_PART = "network"
NETWORK_NEED = "network"


@dataclass(frozen=True)
class RegistrationSlots:
    intents: int
    subscribers: int
    settings: int
    commands: int
    any_of: tuple = ()


@dataclass
class Unit:
    kind: str              # "engine", "theme", "module" or "application"
    name: str
    component: str         # ESP-IDF component name (its directory name)
    path: Path
    depends: list = field(default_factory=list)   # applications: required modules
    optional: list = field(default_factory=list)  # applications: optional modules; others: optional_depends
    needs: list = field(default_factory=list)
    start: bool = False
    foreground: bool = False
    page_step: bool = False
    one_of: list = field(default_factory=list)
    configure: list = field(default_factory=list)
    options: dict = field(default_factory=dict)   # build options the unit needs
    display: dict = None                           # themes: displays drawn for
    startup_error_guide: bool = False              # themes: draw before the UI engine starts
    activities: dict = field(default_factory=dict)  # activity -> default priority
    handles: list = field(default_factory=list)     # applications: module events presented
    claims: bool = False                            # applications: has xiaotai_app_<name>_claim
    commands: list = field(default_factory=list)    # AT command spellings the unit registers
    network_kinds: list = field(default_factory=list)  # engines and modules: network part kinds driven
    registrations: list = field(default_factory=list)

    @property
    def engine(self):
        """Engines and themes come before every base module."""
        return self.kind in ("engine", "theme")


class SelectionError(ValueError):
    pass


def _configure_entries(path, name, entries):
    if not isinstance(entries, list):
        raise SelectionError(f"{path}: configure must be a list")
    for entry in entries:
        extra = set(entry) - CONFIGURE_FIELDS if isinstance(entry, dict) else {"?"}
        form = entry.get("form", "struct") if isinstance(entry, dict) else None
        if extra or form not in CONFIGURE_FORMS:
            raise SelectionError(f"{path}: bad configure entry (unknown fields {sorted(extra)} or form)")
        if not C_IDENTIFIER.match(str(entry.get("function", ""))) or not entry.get("header"):
            raise SelectionError(f"{path}: a configure entry needs a function and a header")
        if form != "call" and not C_TYPE.match(str(entry.get("type", ""))):
            raise SelectionError(f"{path}: configure entry {entry['function']} needs a type")
        if not NAME.match(str(entry.get("section", name))):
            raise SelectionError(f"{path}: configure entry {entry['function']} has a bad section")
        for struct_field, member in entry.get("overrides", {}).items():
            if member not in CONFIG_MEMBERS or not C_IDENTIFIER.match(struct_field):
                raise SelectionError(f"{path}: configure entry {entry['function']} overrides {struct_field} "
                                     f"from {member}, which xiaotai_config_t does not have")
        absent = entry.get("absent_with", {})
        if not isinstance(absent, dict) or not all(
                NAME.match(str(unit)) and isinstance(fields, list) and fields
                and all(isinstance(item, str) and C_IDENTIFIER.match(item) for item in fields)
                for unit, fields in absent.items()):
            raise SelectionError(f"{path}: configure entry {entry['function']}: absent_with maps unit names to "
                                 "lists of parameter fields")
    return list(entries)


def check_absent_with(units, product):
    """configure "absent_with" (build-integration.md section 4): a field the
    entry lists under a unit may not appear in the product's parameters when
    that unit is selected (the clock module refuses its own time zone and time
    servers where the platform core keeps time). Raises SelectionError."""
    selected = {unit.name for unit in units}
    parameters = product.get("parameters", {}) or {}
    for unit in units:
        for entry in unit.configure:
            section = entry.get("section", unit.name)
            values = parameters.get(section)
            if not isinstance(values, dict):
                continue
            for other, fields in entry.get("absent_with", {}).items():
                given = sorted(set(fields) & set(values)) if other in selected else []
                if given:
                    raise SelectionError(f"parameters.{section}: {given} must not be given when the product "
                                         f"selects {other} ({entry['function']})")


def _option_values(path, options):
    if not isinstance(options, dict):
        raise SelectionError(f"{path}: options must be an object")
    for option, value in options.items():
        if not OPTION.match(option) or not isinstance(value, (bool, int, str)):
            raise SelectionError(f"{path}: bad build option {option}")
        if option in REGISTRY_OPTIONS.values():
            raise SelectionError(f"{path}: {option} is generated from selected unit registrations")
    return dict(options)


def _registrations(path, data, optional):
    counters = set(REGISTRY_RANGES)

    def slots(row, group=()):
        for key, (_, limit) in REGISTRY_RANGES.items():
            value = row.get(key, 0)
            if type(value) is not int or not 0 <= value <= limit:
                raise SelectionError(f"{path}: registrations.{key} must be an integer from 0 to {limit}")
        return RegistrationSlots(row.get("intents", 0), row.get("subscribers", 0),
                                 row.get("settings", 0), row.get("commands", 0), tuple(group))

    if not isinstance(data, dict) or set(data) - counters - {"when_selected"}:
        raise SelectionError(f"{path}: registrations takes intents, subscribers, settings, commands and when_selected")
    conditional = data.get("when_selected", [])
    if not isinstance(conditional, list):
        raise SelectionError(f"{path}: registrations.when_selected must be a list")
    declarations = [slots(data)]
    for row in conditional:
        if not isinstance(row, dict) or set(row) - counters - {"any_of"}:
            raise SelectionError(f"{path}: registration condition takes any_of, intents, subscribers, settings and commands")
        group = row.get("any_of")
        if (not isinstance(group, list) or not group
                or any(not isinstance(name, str) or name not in optional for name in group)
                or len(set(group)) != len(group)):
            raise SelectionError(f"{path}: registration condition any_of needs distinct declared optional units")
        declarations.append(slots(row, group))
    return declarations


def _registry_options(units, *, compact_settings=False, compact_commands=False):
    selected = {unit.name for unit in units if unit.kind != "application"}
    # framework.c:initialize always installs system.restart before loading any unit.
    sizes = {"intents": 1, "subscribers": 0, "settings": 0, "commands": 0}
    for unit in units:
        for count in unit.registrations:
            if not count.any_of or selected.intersection(count.any_of):
                for key in sizes:
                    sizes[key] += getattr(count, key)
    names = dict(REGISTRY_OPTIONS)
    if compact_settings:
        names["settings"] = SETTINGS_REGISTRY_OPTION
    if compact_commands:
        names["commands"] = COMMAND_REGISTRY_OPTION
    options = []
    for key, option in names.items():
        minimum, maximum = REGISTRY_RANGES[key]
        if sizes[key] > maximum:
            raise SelectionError(f"{option}: selected units register {sizes[key]}, maximum is {maximum}")
        options.append(f"CONFIG_XIAOTAI_{option}={max(sizes[key], minimum)}")
    return options


def _display_rule(path, rule):
    if rule is None:
        return None
    if not isinstance(rule, dict) or set(rule) - DISPLAY_FIELDS:
        raise SelectionError(f"{path}: display takes {sorted(DISPLAY_FIELDS)}")
    pixel = rule.get("pixel", [])
    if not isinstance(pixel, list) or not all(isinstance(item, str) for item in pixel):
        raise SelectionError(f"{path}: display.pixel must be a list of pixel formats")
    # Themes use the board.json spellings (sdk/xiaotai_hal/needs.json vocabulary).
    known = PART_VOCABULARY.get("display.pixel")
    unknown = [item for item in pixel if known is not None and item not in known]
    if unknown:
        raise SelectionError(f"{path}: display.pixel {unknown} are not pixel formats of board.json "
                             f"({sorted(known)})")
    for key in ("width", "height"):
        span = rule.get(key)
        if span is not None and not (isinstance(span, list) and len(span) == 2
                                     and all(isinstance(item, int) for item in span) and span[0] <= span[1]):
            raise SelectionError(f"{path}: display.{key} must be [minimum, maximum]")
    return dict(rule)


def display_matches(rule, display):
    """True when a theme's display rule covers one display of board.json "parts"."""
    if rule.get("pixel") and display.get("pixel") not in rule["pixel"]:
        return False
    for key in ("width", "height"):
        span = rule.get(key)
        if span is not None and not span[0] <= int(display.get(key, 0)) <= span[1]:
            return False
    return True


def _priority(value):
    return isinstance(value, int) and not isinstance(value, bool) and 1 <= value <= 255


def _activities(path, activities):
    if not isinstance(activities, dict):
        raise SelectionError(f"{path}: activities must map activity names to priorities")
    for activity, priority in activities.items():
        if not NAME.match(activity) or not _priority(priority):
            raise SelectionError(f"{path}: activity {activity} needs a name and a priority 1..255")
    return dict(activities)


def _handles(path, handles, claims):
    if not isinstance(handles, list) or not all(isinstance(name, str) and EVENT_NAME.match(name)
                                                for name in handles) or len(set(handles)) != len(handles):
        raise SelectionError(f"{path}: handles must be distinct module event names (\"call.incoming\")")
    if not isinstance(claims, bool):
        raise SelectionError(f"{path}: claims must be true or false")
    if claims and not handles:
        raise SelectionError(f"{path}: claims without handles: the application presents no event to claim")
    return list(handles)


def _commands(path, commands):
    """AT command spellings a unit registers (spec xiaotai_at.md 4.0); the
    build's vocabulary is the merge over the selected units."""
    if not isinstance(commands, list):
        raise SelectionError(f"{path}: commands must be a list")
    for entry in commands:
        if not isinstance(entry, dict) or set(entry) - COMMAND_FIELDS:
            raise SelectionError(f"{path}: a command takes {sorted(COMMAND_FIELDS)} ({entry!r})")
        name = str(entry.get("command", ""))
        local_query = (AT_LOCAL_QUERY.fullmatch(name) and entry.get("access") == "read"
                       and "verb" not in entry and not entry.get("usage")
                       and "option" in entry and "symbol" in entry)
        if (not (AT_COMMAND.fullmatch(name) or local_query) or entry.get("access") not in COMMAND_ACCESS
                or ("verb" in entry and not AT_VERB.match(str(entry["verb"])))
                or not isinstance(entry.get("usage", ""), str)):
            raise SelectionError(f"{path}: bad command {entry!r} (\"AT+NAME\", verb [A-Z0-9_], access "
                                 f"{'/'.join(COMMAND_ACCESS)})")
        if ("option" in entry) != ("symbol" in entry) or (
                "option" in entry and not (C_CONSTANT.match(str(entry["option"]))
                                           and C_IDENTIFIER.match(str(entry["symbol"])))):
            raise SelectionError(f"{path}: command {entry['command']}: option (XIAOTAI_...) and symbol (the "
                                 "handler prefix) go together")
    return list(commands)


def _network_kinds(path, kinds):
    known = PART_VOCABULARY.get(f"{NETWORK_PART}.kind", {})
    if not isinstance(kinds, list) or len(set(map(str, kinds))) != len(kinds):
        raise SelectionError(f"{path}: network_kinds must be a list of distinct network part kinds")
    unknown = [kind for kind in kinds if kind not in known]
    if unknown:
        raise SelectionError(f"{path}: network_kinds {unknown} are not network part kinds "
                             f"of sdk/xiaotai_hal/needs.json ({sorted(known)})")
    return list(kinds)


def _load(path, kind):
    data = json.loads(path.read_text(encoding="utf-8"))
    allowed = ({"name", "modules", "optional_modules", "needs", "start", "foreground", "configure", "options",
                "activities", "handles", "claims", "commands", "page_step"}
               if kind == "application"
               else {"name", "depends", "optional_depends", "one_of", "needs", "start", "configure", "options",
                     "activities", "commands", "network_kinds"})
    if kind == "theme":
        allowed = (allowed - {"activities", "network_kinds"}) | {"display", "startup_error_guide"}
    allowed.add("registrations")
    extra = set(data) - allowed
    if extra or not NAME.match(str(data.get("name", ""))):
        raise SelectionError(f"{path}: bad manifest (unknown fields {sorted(extra)} or name)")
    unknown = set(data.get("needs", [])) - set(NEEDS)
    if unknown:
        raise SelectionError(f"{path}: unknown needs {sorted(unknown)}")
    application = kind == "application"
    optional = list(data.get("optional_modules" if application else "optional_depends", []))
    page_step = data.get("page_step", False)
    if not isinstance(page_step, bool):
        raise SelectionError(f"{path}: page_step must be a boolean")
    startup_error_guide = data.get("startup_error_guide", False)
    if not isinstance(startup_error_guide, bool):
        raise SelectionError(f"{path}: startup_error_guide must be a boolean")
    return Unit(kind=kind, name=data["name"], component=path.parent.name, path=path.parent,
                depends=list(data.get("modules" if application else "depends", [])),
                optional=optional,
                needs=list(data.get("needs", [])),
                start=bool(data.get("start")), foreground=bool(data.get("foreground")), page_step=page_step,
                one_of=[list(group) for group in data.get("one_of", [])],
                configure=_configure_entries(path, data["name"], data.get("configure", [])),
                options=_option_values(path, data.get("options", {})),
                display=_display_rule(path, data.get("display")),
                startup_error_guide=startup_error_guide,
                activities=_activities(path, data.get("activities", {})),
                handles=_handles(path, data.get("handles", []), data.get("claims", False)),
                claims=bool(data.get("claims", False)),
                commands=_commands(path, data.get("commands", [])),
                network_kinds=_network_kinds(path, data.get("network_kinds", [])),
                registrations=_registrations(path, data.get("registrations", {}), optional))


def catalog(root=ROOT):
    """Engines, themes and modules share one namespace ("module"); applications have their own."""
    root = Path(root)
    units = {"module": {}, "application": {}}
    for kind, pattern in PATTERNS:
        bucket = units["application" if kind == "application" else "module"]
        for path in sorted(root.glob(pattern)):
            unit = _load(path, kind)
            if unit.name in bucket:
                raise SelectionError(f"{kind} {unit.name} declared twice")
            bucket[unit.name] = unit
    for app in units["application"].values():
        for event in app.handles:
            if EVENT_NAME.match(event).group(1) not in units["module"]:
                raise SelectionError(f"application {app.name} handles {event}, but no unit named "
                                     f"{event.split('.')[0]} exists")
    return units


def activity_defaults(modules):
    """Activity -> default priority over the selected engines, modules and
    applications (D47). Several units may begin one activity (call and wechat
    both begin "call") only with the same default."""
    found, owner = {}, {}
    for unit in modules:
        for activity, priority in unit.activities.items():
            if found.get(activity, priority) != priority:
                raise SelectionError(f"activity {activity}: {unit.kind} {unit.name} gives priority {priority}, "
                                     f"{owner[activity]} gives {found[activity]}")
            found[activity] = priority
            owner.setdefault(activity, f"{unit.kind} {unit.name}")
    return found


def resolve(units, applications, extra_modules=(), capabilities=None, needs=None):
    """Applications, engines and modules in dependency order, and their components.

    capabilities: schema-2 board capability names; needs: need names the board
    has (board_needs()). Either one switches the hardware check on."""
    apps = []
    for name in applications:
        if name not in units["application"]:
            raise SelectionError(f"unknown application {name}")
        apps.append(units["application"][name])
    for app in apps:
        for name in app.optional:
            if name not in units["module"]:
                raise SelectionError(f"application {app.name}: optional module {name} does not exist")
    wanted = [name for name in BASE_ENGINES if name in units["module"]]
    for app in apps:
        wanted += app.depends
    wanted += list(extra_modules)

    def walk(follow_optional):
        order, state = [], {}

        def visit(name, chain):
            if name not in units["module"]:
                raise SelectionError(f"{name} (needed by {' -> '.join(chain)}) does not exist")
            if state.get(name) == 2:
                return
            if state.get(name) == 1:
                raise SelectionError(f"dependency cycle: {' -> '.join(chain + [name])}")
            state[name] = 1
            unit = units["module"][name]
            for dependency in unit.depends:
                if unit.engine and not units["module"].get(dependency, unit).engine:
                    raise SelectionError(f"engine {name} must not depend on base module {dependency}")
                visit(dependency, chain + [name])
            for dependency in unit.optional if follow_optional else ():
                if dependency in follow_optional:
                    if unit.engine and not units["module"][dependency].engine:
                        raise SelectionError(f"engine {name} must not depend on base module {dependency}")
                    visit(dependency, chain + [name])
            state[name] = 2
            order.append(unit)

        for name in wanted:
            visit(name, ["product"])
        return order

    # First what is selected, then its order with the optional edges among the selected.
    order = walk(None)
    selected = {unit.name for unit in order}
    if any(unit.optional for unit in order):
        order = walk(selected)
    # Engines never depend on modules, so all of them can come first.
    order = [unit for unit in order if unit.engine] + [unit for unit in order if not unit.engine]
    base = [name for name in BASE_ENGINES if name in units["module"]]
    if [unit.name for unit in order[:len(base)]] != base:
        raise SelectionError(f"the base engines {base} must come first and in this order, but the order begins "
                             f"{[unit.name for unit in order[:len(base)]]}: a base engine depends on another unit")
    activity_defaults(order + apps)
    for unit in order:
        for dependency in unit.optional:
            if dependency not in units["module"]:
                raise SelectionError(f"{unit.kind} {unit.name}: optional dependency {dependency} does not exist")
        for group in unit.one_of:
            if not selected.intersection(group):
                raise SelectionError(f"{unit.kind} {unit.name} needs one of {sorted(group)} in the product")
    have = None
    if needs is not None:
        have = set(needs)
    elif capabilities is not None:
        have = set()
        for capability in capabilities:
            have |= CAPABILITY_NEEDS.get(capability, set())
    if have is not None:
        for unit in apps + order:
            missing = set(unit.needs) - have
            if missing:
                raise SelectionError(f"{unit.kind} {unit.name} needs {sorted(missing)}, which this board lacks")
    components = [unit.component for unit in order + apps]
    return apps, order, components, selected


# ----------------------------------------------------------------- products


def board_needs(board):
    """Need names a board package declares (board.json schema 3, "parts").

    The rules are those of sdk/xiaotai_hal/needs.json, which xiaotai_hal_needs()
    applies to the board's real parts at run time; xiaotai_start() compares the
    two. A need holds when the board has a part of its class whose fields match
    every entry of "where" (a field the part does not give takes its value from
    "defaults"), or, for a memory need, when board.json "memory" says so."""
    parts = board.get("parts", {})
    have = set()
    for rule in NEED_RULES:
        if "memory" in rule:
            if board.get("memory", {}).get(rule["memory"]):
                have.add(rule["name"])
            continue
        defaults = NEED_DEFAULTS.get(rule["part"], {})
        for part in _parts(parts, rule["part"]):
            if all(_field_matches(key, part.get(key, defaults.get(key)), allowed)
                   for key, allowed in rule.get("where", {}).items()):
                have.add(rule["name"])
                break
    return have


def _parts(parts, part_class):
    found = parts.get(part_class, [])
    return [part for part in (found if isinstance(found, list) else [found]) if isinstance(part, dict)]


def _field_matches(key, value, allowed):
    """One "where" entry of a need rule: the part's value is one of `allowed`;
    for "role", a role of one of the families (needs.json)."""
    if key == ROLE_FIELD:
        return isinstance(value, str) and any(value == family or value.startswith(family + ".")
                                              for family in allowed)
    return value in allowed


def check_board(board):
    """The spellings of a board package's parts (board.json "parts") against
    the vocabulary of sdk/xiaotai_hal/needs.json: every field the vocabulary
    lists ("input.kind", "network.kind", "measurement.kind", ...) takes one of
    its spellings, a network part names its kind (the drivers are chosen by
    it), its "wifi" facts are an object, and a role is a dotted name. Returns
    nothing; raises SelectionError on the first fault."""
    parts = board.get("parts", {})
    if not isinstance(parts, dict):
        raise SelectionError("board.json \"parts\" must map part classes to lists of parts")
    for part_class, found in parts.items():
        if not isinstance(found, (list, dict)):
            raise SelectionError(f"board.json parts.{part_class} must be a list of parts")
        for index, part in enumerate(found if isinstance(found, list) else [found]):
            where = f"board.json parts.{part_class}[{index}]"
            if not isinstance(part, dict):
                raise SelectionError(f"{where} must be an object")
            for key, value in part.items():
                words = PART_VOCABULARY.get(f"{part_class}.{key}")
                # A list or object here is a spelling fault too; testing it with
                # "in" would raise TypeError for an unhashable value.
                if words is not None and (not isinstance(value, str) or value not in words):
                    raise SelectionError(f"{where}.{key} is {value!r}, not one of {sorted(words)} "
                                         "(sdk/xiaotai_hal/needs.json vocabulary)")
            role = part.get(ROLE_FIELD)
            if role is not None and not (isinstance(role, str)
                                         and re.match(r"^[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)*$", role)):
                raise SelectionError(f"{where}.role must be a dotted lower-case name (\"door.gate\")")
            if part_class == "display":
                for dimension in ("width", "height"):
                    if dimension in part and (type(part[dimension]) is not int or part[dimension] <= 0):
                        raise SelectionError(f"{where}.{dimension} must be a positive integer")
                if "touch" in part and not isinstance(part["touch"], bool):
                    raise SelectionError(f"{where}.touch must be a boolean")
            if part_class == NETWORK_PART:
                if "kind" not in part:
                    raise SelectionError(f"{where} names no kind: its driver cannot be chosen")
                if "wifi" in part and not isinstance(part["wifi"], dict):
                    raise SelectionError(f"{where}.wifi must be an object of Wi-Fi facts")


def network_drivers(units, selected, board):
    """Names of the units that drive the board's network parts, for a product
    whose selected units (engines, modules, applications) include one that needs
    the network. Each kind the board lists is driven by the selected unit that
    declares it in "network_kinds", or else by the one unit of the catalog that
    does, which the product then selects. No such unit, or several unselected
    ones, is an error. A board without network parts, or a product that does
    not use the network, needs none."""
    if not any(NETWORK_NEED in unit.needs for unit in selected):
        return []
    names = {unit.name for unit in selected}
    drivers = []
    for part in _parts(board.get("parts", {}), NETWORK_PART):
        kind = part.get("kind")
        providers = [unit for unit in units["module"].values() if kind in unit.network_kinds]
        chosen = [unit for unit in providers if unit.name in names] or providers
        if not providers:
            raise SelectionError(f"the board's network part of kind {kind!r} has no driver: no engine or "
                                 "module declares it in \"network_kinds\"")
        if len(chosen) > 1:
            raise SelectionError(f"network kind {kind!r} is driven by {sorted(u.name for u in chosen)}; "
                                 "the product must select one of them")
        if chosen[0].name not in drivers:
            drivers.append(chosen[0].name)
    return drivers


def needs_mask(names):
    mask = 0
    for name in names:
        mask |= NEEDS[name]
    return mask


def _product_needs(product, field, label):
    if field not in product:
        return ()
    values = product[field]
    if (not isinstance(values, list) or not values
            or any(not isinstance(name, str) or name not in NEEDS for name in values)
            or len(values) != len(set(values))):
        raise SelectionError(f"{label}: {field} must be a nonempty list of distinct known board needs")
    return values


def _product_names(product, label):
    name, board = product.get("name"), product.get("board")
    if not (isinstance(name, str) and PRODUCT_NAME.fullmatch(name)
            and isinstance(board, str) and BOARD_NAME.fullmatch(board)):
        raise SelectionError(f"{label}: a product needs a name ([a-z0-9-]) and a board (<vendor>/<board>)")


def load_product(path):
    """A product configuration, checked for shape (not yet against the catalog)."""
    path = Path(path)
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise SelectionError(f"{path}: a product must be an object")
    extra = set(data) - PRODUCT_FIELDS
    if extra or type(data.get("schema")) is not int or data["schema"] != 1:
        raise SelectionError(f"{path}: bad product (unknown fields {sorted(extra)} or schema)")
    _product_names(data, path)
    for field in ("needs_all", "needs_any"):
        _product_needs(data, field, path)
    for key in ("applications", "modules", "sdkconfig"):
        value = data.get(key, [])
        if not isinstance(value, list) or not all(isinstance(item, str) for item in value):
            raise SelectionError(f"{path}: {key} must be a list of strings")
    if not data.get("applications") and not data.get("modules"):
        raise SelectionError(f"{path}: a product selects at least one application or module")
    home = data.get("home")
    if home is not None and home not in data.get("applications", []):
        raise SelectionError(f"{path}: home {home} is not one of the product's applications")
    if not isinstance(data.get("priorities", {}), dict):
        raise SelectionError(f"{path}: priorities must map activity names to 1..255")
    for activity, priority in data.get("priorities", {}).items():
        if not NAME.match(activity) or not _priority(priority):
            raise SelectionError(f"{path}: priority of {activity} must be 1..255")
    preemptions = data.get("preemptions", [])
    pairs = set()
    if not isinstance(preemptions, list):
        raise SelectionError(f"{path}: preemptions must be a list of {sorted(PREEMPTION_FIELDS)}")
    for rule in preemptions:
        if (not isinstance(rule, dict) or set(rule) != PREEMPTION_FIELDS
                or not NAME.match(str(rule["activity"])) or not NAME.match(str(rule["over"]))
                or rule["activity"] == rule["over"] or not isinstance(rule["preempts"], bool)):
            raise SelectionError(f"{path}: a preemption is {{\"activity\", \"over\", \"preempts\": true|false}} "
                                 f"between two different activities ({rule!r})")
        if (rule["activity"], rule["over"]) in pairs:
            raise SelectionError(f"{path}: preemption {rule['activity']} over {rule['over']} given twice")
        pairs.add((rule["activity"], rule["over"]))
    restart = data.get("start_failure_restart_ms", 0)
    if not isinstance(restart, int) or isinstance(restart, bool) or not START_HALT <= restart <= 0x7FFFFFFF:
        raise SelectionError(f"{path}: start_failure_restart_ms is -1 (stay up), 0 (the default delay) or a delay")
    if not isinstance(data.get("local_on_storage_boot_failure", False), bool):
        raise SelectionError(f"{path}: local_on_storage_boot_failure must be a boolean")
    _option_values(path, data.get("options", {}))
    for line, package in data.get("tirtc", {}).items():
        if not re.match(r"^[0-9]+\.[0-9]+$", line) or not re.match(r"^[a-z0-9][a-z0-9-]*$", str(package)):
            raise SelectionError(f"{path}: tirtc maps an ESP-IDF line (6.0) to a package name or \"generic\"")
    for line in data.get("sdkconfig", []):
        if not line.startswith("CONFIG_") or line.startswith("CONFIG_XIAOTAI_"):
            raise SelectionError(f"{path}: sdkconfig takes platform options only; XiaoTai options go to "
                                 f"\"options\" ({line})")
    if not isinstance(data.get("parameters", {}), dict):
        raise SelectionError(f"{path}: parameters must be an object")
    _framework_parameters(data.get("parameters", {}).get("framework", {}))
    budget = data.get("budget", {})
    if (not isinstance(budget, dict) or set(budget) - BUDGET_FIELDS
            or not all(isinstance(value, int) and not isinstance(value, bool) and value > 0
                       for value in budget.values())):
        raise SelectionError(f"{path}: budget takes {sorted(BUDGET_FIELDS)} as positive byte counts")
    for name, variant in data.get("variants", {}).items():
        if not PRODUCT_NAME.match(name) or not isinstance(variant, dict) or set(variant) - VARIANT_FIELDS:
            raise SelectionError(f"{path}: bad variant {name}")
        sections = variant.get("parameters", {})
        if not isinstance(sections, dict) or not all(isinstance(values, dict) for values in sections.values()):
            raise SelectionError(f"{path}: variant {name}: parameters must map sections to objects")
        absent = set(variant.get("without", [])) - set(data.get("modules", []))
        if absent:
            raise SelectionError(f"{path}: variant {name} removes {sorted(absent)}, which the product's "
                                 f"\"modules\" do not list")
        _option_values(f"{path}: variant {name}", variant.get("options", {}))
    return data


def apply_variant(product, name):
    """The product as one of its declared variants builds it: the same product
    without some of its extra modules (and their parameters), with other
    build options, or with some fields of parameter sections replaced (a
    development console). Applications and the board never change."""
    if name not in product.get("variants", {}):
        raise SelectionError(f"product {product['name']} has no variant {name}")
    variant = product["variants"][name]
    removed = set(variant.get("without", []))
    result = dict(product)
    result["modules"] = [module for module in product.get("modules", []) if module not in removed]
    parameters = {section: values for section, values in product.get("parameters", {}).items()
                  if section not in removed}
    for section, values in variant.get("parameters", {}).items():
        base = parameters.get(section, {})
        if not isinstance(base, dict):
            raise SelectionError(f"variant {name}: parameters.{section} of the product is not an object")
        parameters[section] = {**base, **values}
    result["parameters"] = parameters
    result["options"] = {**product.get("options", {}), **variant.get("options", {})}
    return result


def _c_string(text):
    out = []
    for byte in text.encode("utf-8"):
        if byte in (0x22, 0x5c):
            out.append("\\" + chr(byte))
        elif 0x20 <= byte < 0x7f:
            out.append(chr(byte))
        else:
            out.append(f"\\{byte:03o}")
    return '"' + "".join(out) + '"'


class _Glue:
    """Static objects and statements of the product glue."""

    def __init__(self, board=None):
        self.objects = []      # C definitions, in emission order
        self.known = {}        # (type, body) -> symbol
        self.counter = 0
        self.board = board or {}

    def symbol(self, ctype, body, array=False):
        key = (ctype, body, array)
        if key not in self.known:
            self.counter += 1
            name = f"glue_{self.counter}"
            suffix = "[]" if array else ""
            # "T const" so that an element type such as "const char *" yields constant elements.
            self.objects.append(f"static {ctype} const {name}{suffix} = {body};\n")
            self.known[key] = name
        return self.known[key]

    def reference(self, text, where):
        """"@board.<path>": a value of the board package's manifest."""
        found = self.board
        for key in text[len(BOARD_REFERENCE):].split("."):
            if not isinstance(found, dict) or key not in found:
                raise SelectionError(f"{where}: the board manifest has no {text[1:]}")
            found = found[key]
        if isinstance(found, (dict, list)):
            raise SelectionError(f"{where}: {text[1:]} is not a single value")
        return found

    def value(self, value, entry, where, name=None):
        """C initializer text of a JSON value; `name` is the field it initializes."""
        if isinstance(value, str) and value.startswith(BOARD_REFERENCE):
            value = self.reference(value, where)
        if isinstance(value, bool):
            return "true" if value else "false"
        if isinstance(value, int):
            return str(value)
        if value is None:
            return "NULL"
        if isinstance(value, str):
            return value if C_CONSTANT.match(value) else _c_string(value)
        if isinstance(value, dict):
            pointer = entry.get("pointers", {}).get(name)
            body = self.struct(value, entry, where)
            return f"&{self.symbol(pointer, body)}" if pointer else body
        if isinstance(value, list):
            items = ", ".join(self.value(item, entry, where) for item in value)
            return "{" + items + "}"
        raise SelectionError(f"{where}: {value!r} cannot be written as a C value")

    def struct(self, data, entry, where):
        fields = []
        for name, value in data.items():
            if not C_IDENTIFIER.match(name):
                raise SelectionError(f"{where}: {name} is not a field name")
            array = entry.get("arrays", {}).get(name)
            if array is not None and isinstance(value, list):
                items = [self.value(item, entry, f"{where}.{name}") for item in value]
                if array.get("terminated"):
                    items.append("NULL")
                symbol = self.symbol(array["type"], "{" + ", ".join(items) + "}", array=True) if items else "NULL"
                fields.append(f".{name} = {symbol}")
                if array.get("count"):
                    fields.append(f".{array['count']} = {len(value)}")
            else:
                fields.append(f".{name} = {self.value(value, entry, f'{where}.{name}', name)}")
        return "{" + ", ".join(fields) + "}"


def _without_external_ram(board):
    return board is not None and board.get("memory", {}).get("external_ram") is False


def at_parameters(product, board):
    """Resolve console attachment and AT table storage independently."""
    values = dict(product.get("parameters", {}).get("at", {}))
    if _without_external_ram(board):
        values.setdefault("no_platform_console", True)
        if values["no_platform_console"] is True:
            values.setdefault("omit_command_tables", True)
    for name in ("no_platform_console", "omit_command_tables"):
        if name in values and type(values[name]) is not bool:
            raise SelectionError(f"parameters.at.{name}: expected a boolean")
    if values.get("omit_command_tables") and (not values.get("no_platform_console") or values.get("follow_log")):
        raise SelectionError("parameters.at.omit_command_tables: requires no platform console or log reader")
    return values


def _framework_parameters(values):
    where = "parameters.framework"
    if not isinstance(values, dict):
        raise SelectionError(f"{where}: expected an object")
    extra = set(values) - {"reserve_owner_terminal"}
    if extra:
        raise SelectionError(f"{where}: unknown fields {sorted(extra)}")
    if not isinstance(values.get("reserve_owner_terminal", False), bool):
        raise SelectionError(f"{where}.reserve_owner_terminal: expected a boolean")


def _glue(apps, modules, product, board=None):
    """Definitions and the body of xiaotai_selected_configure() for a product."""
    glue = _Glue(board)
    check_absent_with(list(modules) + list(apps), product)
    parameters = dict(product.get("parameters", {}))
    _framework_parameters(parameters.pop("framework", {}))
    compact = _without_external_ram(board)
    headers, calls = [], []
    for unit in list(modules) + list(apps):
        for entry in unit.configure:
            section = entry.get("section", unit.name)
            form = entry.get("form", "struct")
            function = entry["function"]
            given = section in parameters
            values = parameters.pop(section, None)
            where = f"parameters.{section}"
            if entry["header"] not in headers:
                headers.append(entry["header"])
            if form == "call":
                if given:
                    raise SelectionError(f"{where}: {function} takes no values")
                calls.append(f"    err = {function}();\n")
            elif form == "table":
                pairs = entry.get("pairs")
                if isinstance(values, dict) and pairs:
                    values = [{pairs[0]: key, pairs[1]: value} for key, value in values.items()]
                if given and not isinstance(values, list):
                    raise SelectionError(f"{where}: {function} takes a list")
                if unit.name == "media" and section == "media":
                    key = "task.capture.compact_stack"
                    choices = [item.get("value") for item in values or ()
                               if isinstance(item, dict) and item.get("name") == key]
                    if len(choices) > 1 or any(type(value) is not int or value not in (0, 1) for value in choices):
                        raise SelectionError(f"{where}.{key}: expected one integer 0 or 1")
                    if compact and not choices:
                        values = [*(values or ()), {"name": key, "value": 1}]
                if values:
                    body = "{" + ", ".join(glue.value(item, entry, where) for item in values) + "}"
                    calls.append(f"    err = {function}({glue.symbol(entry['type'], body, array=True)}, {len(values)});\n")
                elif entry.get("always"):
                    calls.append(f"    err = {function}(NULL, 0);\n")
            else:
                if given and not isinstance(values, dict):
                    raise SelectionError(f"{where}: {function} takes an object")
                if unit.name == "ui" and section == "ui" and values:
                    values = {key: value for key, value in values.items() if key != "show_start_failure"}
                if unit.name == "at" and section == "at":
                    values = at_parameters(product, board)
                if unit.name == "transport" and section == "transport":
                    if compact:
                        values = dict(values or {})
                        values.setdefault("vendor_idle_stack_bytes", 4096)
                    if values and "vendor_idle_stack_bytes" in values:
                        size = values["vendor_idle_stack_bytes"]
                        if type(size) is not int or (size != 0 and not (4096 <= size <= 8192 and size % 16 == 0)):
                            raise SelectionError(
                                f"{where}.vendor_idle_stack_bytes: expected 0 or 4096..8192 aligned to 16")
                overrides = entry.get("overrides", {})
                symbol = glue.symbol(entry["type"], glue.struct(values, entry, where)) if values else None
                if overrides:
                    lines = [f"        {entry['type']} value;\n"]
                    lines.append(f"        value = {symbol};\n" if symbol
                                 else "        memset(&value, 0, sizeof(value));\n")
                    for struct_field, member in overrides.items():
                        lines.append(f"        if (config->{member}) value.{struct_field} = config->{member};\n")
                    lines.append(f"        err = {function}(&value);\n")
                    calls.append("    {\n" + "".join(lines) + "    }\n")
                elif symbol:
                    calls.append(f"    err = {function}(&{symbol});\n")
                elif entry.get("always"):
                    calls.append(f"    err = {function}(NULL);\n")
    if parameters:
        raise SelectionError(f"parameters for {sorted(parameters)}, but nothing the product selects takes them")
    return glue, headers, calls


def _c_list(symbol, names):
    items = ", ".join(f'"{name}"' for name in names)
    return f"static const char *const {symbol}[] = {{{items}{', ' if items else ''}NULL}};\n"


def _needs(unit):
    return " | ".join(f"0x{NEEDS[need]:x}u" for need in unit.needs) or "0"


def _c_priorities(symbol, table):
    rows = "".join(f'    {{"{activity}", {priority}}},\n' for activity, priority in table.items())
    return f"static const xiaotai_priority_t {symbol}[] = {{\n{rows}    {{NULL, 0}},\n}};\n"


def _startup_error_guide(modules, product, board):
    """Bind the opted-in recovery page before any runtime UI state exists."""
    ui = product.get("parameters", {}).get("ui", {})
    if not isinstance(ui, dict):
        raise SelectionError("parameters.ui: expected an object")
    enabled = ui.get("show_start_failure", False)
    where = "parameters.ui.show_start_failure"
    if not isinstance(enabled, bool):
        raise SelectionError(f"{where}: expected a boolean")
    if not enabled:
        return None
    displays = _parts((board or {}).get("parts", {}), "display")
    name = ui.get("theme")
    if name is not None and not isinstance(name, str):
        raise SelectionError("parameters.ui.theme: expected a string")
    candidates = [unit for unit in modules if unit.kind == "theme" and unit.startup_error_guide
                  and (not name or unit.name == THEME_PREFIX + name) and displays
                  and unit.display is not None and display_matches(unit.display, displays[0])]
    if len(candidates) != 1:
        raise SelectionError(f"{where}: select one theme able to draw a startup error on the first display")
    return f"xiaotai_engine_{candidates[0].name}_startup_error_guide"


def generate(apps, modules, product=None, board=None):
    """C source defining the selected engines, modules and applications.

    With a product (load_product) the file also defines the product glue
    xiaotai_selected_configure(), the product's configuration
    xiaotai_product_config (home, priorities, preemption rules, start-failure
    policy) and xiaotai_board_declared_needs, and includes xiaotai/selection.h.
    board is the board package's manifest: its "parts" give the declared needs,
    and parameters may refer to its values ("@board...")."""
    out = ["// Generated by scripts/sdk_selection.py from the selected manifests. Do not edit.\n"]
    if product is None:
        out.append('#include <stddef.h>\n#include "xiaotai/framework.h"\n\n')
    else:
        glue, headers, calls = _glue(apps, modules, product, board)
        out.append('#include <stddef.h>\n#include <string.h>\n#include "xiaotai/selection.h"\n')
        out += [f'#include "{header}"\n' for header in headers]
        out.append("\n")
    for unit in modules:
        n = unit.name
        prefix = "xiaotai_engine" if unit.engine else "xiaotai_module"
        out.append(f"xiaotai_err_t {prefix}_{n}_init(void);\n")
        if unit.start:
            out.append(f"void {prefix}_{n}_start(void);\n")
        out.append(_c_list(f"module_{n}_depends", unit.depends))
        activities = "NULL"
        if unit.activities:
            activities = f"module_{n}_activities"
            out.append(_c_priorities(activities, unit.activities))
        start = f"{prefix}_{n}_start" if unit.start else "NULL"
        out.append(f"static const xiaotai_module_t module_{n} = {{\n"
                   f'    .name = "{n}", .depends = module_{n}_depends, .needs = {_needs(unit)},\n'
                   f"    .init = {prefix}_{n}_init, .start = {start}, .activities = {activities},\n}};\n\n")
    for app in apps:
        n = app.name
        out.append(f"xiaotai_err_t xiaotai_app_{n}_init(void);\n")
        if app.start:
            out.append(f"void xiaotai_app_{n}_start(void);\n")
        if app.foreground:
            out.append(f"void xiaotai_app_{n}_foreground(bool foreground);\n")
        if app.page_step:
            out.append(f"xiaotai_err_t xiaotai_app_{n}_page_step(int direction);\n")
        if app.claims:
            out.append(f"bool xiaotai_app_{n}_claim(const xiaotai_event_t *event);\n")
        out.append(_c_list(f"app_{n}_modules", app.depends))
        out.append(_c_list(f"app_{n}_optional", app.optional))
        handles = "NULL"
        if app.handles:
            handles = f"app_{n}_handles"
            out.append(_c_list(handles, app.handles))
        activities = "NULL"
        if app.activities:
            activities = f"app_{n}_activities"
            out.append(_c_priorities(activities, app.activities))
        start = f"xiaotai_app_{n}_start" if app.start else "NULL"
        foreground = f"xiaotai_app_{n}_foreground" if app.foreground else "NULL"
        page_step = f"xiaotai_app_{n}_page_step" if app.page_step else "NULL"
        claim = f"xiaotai_app_{n}_claim" if app.claims else "NULL"
        out.append(f"static const xiaotai_application_t app_{n} = {{\n"
                   f'    .name = "{n}", .modules = app_{n}_modules, .optional_modules = app_{n}_optional,\n'
                   f"    .needs = {_needs(app)}, .init = xiaotai_app_{n}_init, .start = {start},\n"
                   f"    .foreground = {foreground}, .handles = {handles}, .claim = {claim},\n"
                   f"    .activities = {activities}, .page_step = {page_step},\n}};\n\n")
    module_refs = ", ".join(f"&module_{unit.name}" for unit in modules)
    app_refs = ", ".join(f"&app_{app.name}" for app in apps)
    out.append("const xiaotai_module_t *const xiaotai_selected_modules[] = "
               f"{{{module_refs}{', ' if module_refs else ''}NULL}};\n")
    out.append(f"const size_t xiaotai_selected_module_count = {len(modules)};\n")
    out.append("const xiaotai_application_t *const xiaotai_selected_applications[] = "
               f"{{{app_refs}{', ' if app_refs else ''}NULL}};\n")
    out.append(f"const size_t xiaotai_selected_application_count = {len(apps)};\n")
    if product is None:
        return "".join(out)

    out.append("\n/* Product glue: the product's values for the selected units. */\n")
    out += glue.objects
    out.append("\nxiaotai_err_t xiaotai_selected_configure(const xiaotai_config_t *config)\n{\n"
               "    xiaotai_err_t err = XIAOTAI_OK;\n    (void)config;\n")
    for call in calls:
        out.append(call + "    if (err != XIAOTAI_OK) return err;\n")
    out.append("    return err;\n}\n\n")
    priorities = product.get("priorities", {})
    if priorities:
        out.append(_c_priorities("product_priorities", priorities))
    preemptions = product.get("preemptions", [])
    if preemptions:
        rows = "".join(f'    {{"{rule["activity"]}", "{rule["over"]}", {"true" if rule["preempts"] else "false"}}},\n'
                       for rule in preemptions)
        out.append(f"static const xiaotai_preemption_t product_preemptions[] = {{\n{rows}"
                   "    {NULL, NULL, false},\n};\n")
    home = f'"{product["home"]}"' if product.get("home") else "NULL"
    restart = product.get("start_failure_restart_ms", 0)
    local_storage = "    .local_on_storage_boot_failure = true,\n" if product.get("local_on_storage_boot_failure") else ""
    terminal = product.get("parameters", {}).get("framework", {}).get("reserve_owner_terminal")
    terminal_slot = "    .reserve_owner_terminal = true,\n" if terminal else ""
    guide = _startup_error_guide(modules, product, board)
    if guide:
        out.append(f"xiaotai_err_t {guide}(xiaotai_err_t error);\n")
    local_guide = f"    .startup_error_guide = {guide},\n" if guide else ""
    out.append("const xiaotai_config_t xiaotai_product_config = {\n"
               f"    .home = {home}, .priorities = {'product_priorities' if priorities else 'NULL'},\n"
               f"    .preemptions = {'product_preemptions' if preemptions else 'NULL'},\n"
               f"    .start_failure_restart_ms = {'XIAOTAI_START_HALT' if restart == START_HALT else restart},\n"
               f"{local_storage}{terminal_slot}{local_guide}}};\n")
    if "parts" not in (board or {}):
        raise SelectionError(f"board of product {product['name']}: its manifest has no \"parts\", so the needs "
                             "it declares (xiaotai_board_declared_needs) are unknown")
    out.append("\n/* XIAOTAI_NEED_* the board package's manifest declares; xiaotai_start() compares\n"
               " * them with the needs its parts give. */\n"
               f"const uint32_t xiaotai_board_declared_needs = 0x{needs_mask(board_needs(board)):x}u;\n")
    return "".join(out)


def generate_header(product, needs=None):
    """xiaotai_product.h: what the product's main and the board package include."""
    out = ["// Generated by scripts/sdk_selection.py from the product configuration. Do not edit.\n",
           '#pragma once\n#include "xiaotai/xiaotai.h"\n\n',
           f'#define XIAOTAI_PRODUCT_NAME "{product["name"]}"\n',
           f'#define XIAOTAI_PRODUCT_BOARD "{product["board"]}"\n']
    if needs is not None:
        out.append("/* XIAOTAI_NEED_* the board package declares; xiaotai_hal_init() compares the\n"
                   " * needs of the real parts with it. */\n"
                   f"#define XIAOTAI_BOARD_DECLARED_NEEDS 0x{needs_mask(needs):x}u\n")
    internal = product.get("budget", {}).get("internal_bytes")
    if internal is not None:
        out.append("/* Internal RAM the product may hold once started (task stacks and buffers of\n"
                   " * every selected unit); the whole-product host test fails above it. */\n"
                   f"#define XIAOTAI_PRODUCT_INTERNAL_BUDGET_BYTES {internal}u\n")
    baseline = product.get("budget", {}).get("baseline_bytes")
    if baseline is not None:
        out.append("/* The same figure for the firmware this product replaces, measured the same\n"
                   " * way; the whole-product host test prints the difference. */\n"
                   f"#define XIAOTAI_PRODUCT_INTERNAL_BASELINE_BYTES {baseline}u\n")
    out.append("\n#ifdef __cplusplus\nextern \"C\" {\n#endif\n"
               "/* Passed to xiaotai_start() by the product's main. */\n"
               "extern const xiaotai_config_t xiaotai_product_config;\n"
               "#ifdef __cplusplus\n}\n#endif\n")
    return "".join(out)


def component_options(root, components):
    """Build option names declared by the selected components."""
    known = set()
    for directory in components.values():
        kconfig = Path(root) / directory / "Kconfig"
        if kconfig.is_file():
            known |= set(re.findall(r"^\s*config\s+XIAOTAI_([A-Z0-9_]+)\s*$", kconfig.read_text(encoding="utf-8"),
                                    re.M))
    return known


def _options(root, product, components, units=(), board=None):
    """sdkconfig lines checked against the selected components' Kconfig files."""
    known = component_options(root, components)
    options = {}
    for unit in units:
        for option, value in unit.options.items():
            if options.get(option, value) != value:
                raise SelectionError(f"build option {option}: {unit.kind} {unit.name} needs {value!r}, "
                                     f"another selected unit needs {options[option]!r}")
            options[option] = value
    for option, value in _option_values(product["name"], product.get("options", {})).items():
        if options.get(option, value) != value:
            raise SelectionError(f"build option {option}: the product sets {value!r}, but a selected unit "
                                 f"needs {options[option]!r}")
        options[option] = value
    # A display of 800 x 480 and more is read from further away: the colour
    # themes draw it with their 26 px face unless the product says otherwise.
    displays = _parts((board or {}).get("parts", {}), "display")
    if (LARGE_TEXT_OPTION in known and displays and int(displays[0].get("width", 0)) >= 800
            and int(displays[0].get("height", 0)) >= 480):
        options.setdefault(LARGE_TEXT_OPTION, True)
    # The door's own pages start at 440 x 300; a smaller display never shows them.
    if (DOOR_TEMPLATES_OPTION in known and displays and (int(displays[0].get("width", 0)) < 440
            or int(displays[0].get("height", 0)) < 300)):
        options.setdefault(DOOR_TEMPLATES_OPTION, False)
    compact = _without_external_ram(board)
    if options.get(TIRTC_IDLE_STACK_COMPACT) is True:
        if not compact or board.get("target") != "esp32c3" or product.get("tirtc") != {"6.0": "xmini-mini"}:
            raise SelectionError(f"{TIRTC_IDLE_STACK_COMPACT}: requires ESP32-C3 without external RAM and 6.0/xmini-mini")
        transport = product.get("parameters", {}).get("transport", {})
        size = transport.get("vendor_idle_stack_bytes", 4096) if isinstance(transport, dict) else None
        if type(size) is not int or size != 4096:
            raise SelectionError(f"{TIRTC_IDLE_STACK_COMPACT}: requires vendor_idle_stack_bytes=4096; "
                                 "disable the profile to use a different size")
    if compact:
        compact_defaults = {"KV_WORKER_STACK_BYTES": 3072, "PLATFORM_SIGNALING_EVENT_STACK_BYTES": 3072,
                            "FRAMEWORK_COMPACT_SUBSCRIBERS": True, "FRAMEWORK_MAX_DIAGNOSTICS": 32,
                            "PORT_HTTP_DNS_ON_DEMAND": True,
                            "TIRTC_SESSION_STACK_9216": True,
                            "AUDIO_RETAIN_I2S_DMA": True,
                            "UI_CAPTURE": False, "MEDIA_TRACE": False, "MEDIA_TRACE_ARRIVALS": False,
                            "MEDIA_TEST_COMMANDS": False, "MEDIA_SOUND_COMMANDS": False}
        if not options.get("PORT_MQTT_NATIVE_OVERLAY"):
            compact_defaults["PORT_MQTT_PAYLOAD_ON_DEMAND"] = True
        # Companion bring-up and update run on the network worker's larger stack.
        networks = board.get("parts", {}).get("network", [])
        if not any(part["kind"] == "wifi_coprocessor" for part in networks):
            compact_defaults["NET_WORKER_STACK_BYTES"] = 3584
        for option, value in compact_defaults.items():
            if option in known:
                options.setdefault(option, value)
        if "FRAMEWORK_OWNER_ON_MAIN" in known:
            options.setdefault("FRAMEWORK_OWNER_ON_MAIN", True)
        if "FRAMEWORK_MAX_MODULES" in known and "FRAMEWORK_MAX_MODULES" not in options:
            # The framework only orders the generated modules array; apps are separate.
            count = sum(unit.kind != "application" for unit in units)
            if count > 255:  # Match the framework Kconfig range.
                raise SelectionError(f"FRAMEWORK_MAX_MODULES: {count} selected units exceed 255")
            options["FRAMEWORK_MAX_MODULES"] = max(8, count)
    # The network worker's stack follows the board's network part: Wi-Fi on a coprocessor needs the larger
    # one (its bring-up verifies and writes firmware images). Stated in every build request rather than left
    # to a Kconfig default: a default does not reach a build directory configured before it changed, and a
    # platform symbol it would be keyed to may change its meaning between ESP-IDF lines.
    if "NET_WORKER_STACK_BYTES" in known:
        coprocessor = any(part.get("kind") == "wifi_coprocessor" for part in _parts(board.get("parts", {}), "network"))
        options.setdefault("NET_WORKER_STACK_BYTES", 8192 if coprocessor else 4096)
    lines = []
    for option, value in options.items():
        if option not in known:
            raise SelectionError(f"build option {option}: no component in this product has "
                                 f"CONFIG_XIAOTAI_{option}")
        if option in ("FRAMEWORK_OWNER_ON_MAIN", "TIRTC_SESSION_STACK_9216", TIRTC_IDLE_STACK_COMPACT) and type(value) is not bool:
            raise SelectionError(f"build option {option}: expected a boolean")
        if isinstance(value, bool):
            text = "y" if value else "n"
        elif isinstance(value, int):
            text = str(value)
        else:
            text = _c_string(value)
        lines.append(f"CONFIG_XIAOTAI_{option}={text}")
    compact_settings = compact and SETTINGS_REGISTRY_OPTION in known and SETTINGS_REGISTRY_OPTION not in options
    compact_commands = compact and COMMAND_REGISTRY_OPTION in known and COMMAND_REGISTRY_OPTION not in options
    return lines + _registry_options(units, compact_settings=compact_settings, compact_commands=compact_commands)


def _sdkconfig(product, units):
    """Platform defaults follow selected units; explicit product values override them."""
    defaults = {}
    for unit in units:
        path = unit.path / "sdkconfig.defaults"
        if not path.is_file():
            continue
        for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            line = raw.strip()
            disabled = re.fullmatch(r"# (CONFIG_[A-Z0-9_]+) is not set", line)
            if disabled:
                line = f"{disabled[1]}=n"
            elif not line or line.startswith("#"):
                continue
            key, sep, value = line.partition("=")
            if not sep or not value or not re.fullmatch(r"CONFIG_(?!XIAOTAI_)[A-Z0-9_]+", key):
                raise SelectionError(f"{path}:{number}: expected a platform sdkconfig default")
            if defaults.get(key, value) != value:
                raise SelectionError(f"{path}:{number}: conflicting selected unit defaults for {key}")
            defaults[key] = value
    # Preserve existing product order (including the IDF last-value-wins rule).
    explicit = list(product.get("sdkconfig", []))
    explicit_keys = {line.partition("=")[0] for line in explicit}
    return [f"{key}={value}" for key, value in defaults.items() if key not in explicit_keys] + explicit


def check_themes(modules, board):
    """Selected themes against the board's displays (board.json "parts.display").

    Only themes whose manifest has a "display" rule take part; the run-time
    choice (xiaotai_ui) still picks the best match among them."""
    themes = [unit for unit in modules if unit.kind == "theme" and unit.display is not None]
    if not themes or "parts" not in board:
        return
    displays = _parts(board["parts"], "display")
    for theme in themes:
        if not any(display_matches(theme.display, display) for display in displays):
            raise SelectionError(f"theme {theme.name} draws for none of the board's displays {displays}")
    for display in displays:
        if not any(display_matches(theme.display, display) for theme in themes):
            raise SelectionError(f"no selected theme draws for the board's display {display}")


def check_theme_parameters(units, product, board):
    """A theme the product names in its parameters (the struct field a
    configure entry overrides from xiaotai_config_t.theme) must be a selected
    theme unit "theme_<name>" (xiaotai/ui_theme.h) and, when that theme has a
    display rule and the board lists displays, draw for the board's first
    display, which the UI system starts it on. The run-time override of
    xiaotai_start() cannot be checked here; the UI system refuses a theme that
    does not fit (xiaotai_ui_info_t.theme_refused)."""
    themes = {unit.name: unit for unit in units if unit.kind == "theme"}
    displays = _parts(board.get("parts", {}), "display")
    parameters = product.get("parameters", {})
    for unit in units:
        for entry in unit.configure:
            section = entry.get("section", unit.name)
            for struct_field, member in entry.get("overrides", {}).items():
                values = parameters.get(section)
                name = values.get(struct_field) if isinstance(values, dict) else None
                if member != THEME_MEMBER or not isinstance(name, str) or not name:
                    continue
                where = f"parameters.{section}.{struct_field}"
                theme = themes.get(THEME_PREFIX + name)
                if theme is None:
                    raise SelectionError(f"{where} names theme {name!r}, but the product selects no theme unit "
                                         f"{THEME_PREFIX}{name} (selected: {sorted(themes)})")
                if theme.display is not None and displays and not display_matches(theme.display, displays[0]):
                    raise SelectionError(f"{where}: theme {theme.name} does not draw for the board's first "
                                         f"display {displays[0]}")


def vocabulary(units):
    """The AT vocabulary a product's selected units register: the merge of
    their manifests' "commands" (spec xiaotai_at.md 4.0), one entry per
    spelling (command, verb and argument form: "AT+SET [<name>]" reads and
    "AT+SET <name> <value>" writes, so they are two spellings with their own
    access), with the units that register it, sorted. Several units may
    register one spelling of a shared command (both call modules own ACCEPT),
    but they must agree on its access and on the build option that compiles
    it: the console script's permission and the link-time check are generated
    per spelling. The bare "AT" is the console's syntax itself (xiaotai_at.md
    4.0, 4.1): no manifest can declare it (AT_COMMAND requires "AT+"), yet
    every console answers it, so it is in the vocabulary whenever the console
    engine is selected."""
    merged = {}
    for unit in units:
        if unit.kind == "engine" and unit.name == CONSOLE_ENGINE:
            merged[(BARE_AT, "", "")] = {"command": BARE_AT, "verb": None, "usage": "", "access": "read",
                                         "option": None, "symbol": None, "units": [unit.name]}
    for unit in units:
        for entry in unit.commands:
            key = (entry["command"], entry.get("verb", ""), entry.get("usage", ""))
            found = merged.get(key)
            if found is None:
                merged[key] = found = {"command": entry["command"], "verb": entry.get("verb"),
                                       "usage": entry.get("usage", ""), "access": entry["access"],
                                       "option": entry.get("option"), "symbol": entry.get("symbol"),
                                       "units": []}
            elif (found["access"], found["option"]) != (entry["access"], entry.get("option")):
                spelling = " ".join(part for part in key if part)
                raise SelectionError(f"{spelling}: {unit.kind} {unit.name} declares access {entry['access']} "
                                     f"option {entry.get('option')}, {found['units'][0]} declares access "
                                     f"{found['access']} option {found['option']}")
            # Applications as in XIAOTAI_SELECTED: their names are a namespace of their own.
            found["units"].append(f"app.{unit.name}" if unit.kind == "application" else unit.name)
    return [merged[key] for key in sorted(merged)]


def missing_configure(root=ROOT):
    """Configure functions declared in a unit's public headers that its
    manifest does not list: the product glue would never call them.
    Returns [(manifest path, function)]."""
    root = Path(root)
    missing = []
    units = catalog(root)
    for unit in list(units["module"].values()) + list(units["application"].values()):
        listed = {entry["function"] for entry in unit.configure}
        for header in sorted((unit.path / "include").rglob("*.h")):
            for function in CONFIGURE_DECLARATION.findall(header.read_text(encoding="utf-8")):
                if function not in listed:
                    missing.append((unit.path.relative_to(root).as_posix(), function))
    return missing


def missing_activities(root=ROOT):
    """Activities named in a unit's public headers (XIAOTAI_<UNIT>_ACTIVITY)
    that its manifest's "activities" does not declare: the framework would
    rank them 1 whatever the default ordering says. Returns [(manifest path,
    activity)]."""
    root = Path(root)
    missing = []
    units = catalog(root)
    for unit in list(units["module"].values()) + list(units["application"].values()):
        for header in sorted((unit.path / "include").rglob("*.h")):
            for activity in ACTIVITY_DECLARATION.findall(header.read_text(encoding="utf-8")):
                if activity not in unit.activities:
                    missing.append((unit.path.relative_to(root).as_posix(), activity))
    return missing


CMAKE_SET = re.compile(r"^\s*set\(([A-Za-z_][A-Za-z0-9_]*)\b([^)]*)\)", re.M)
CMAKE_REGISTER = re.compile(r"idf_component_register\(([^)]*)\)", re.S)
CMAKE_KEYWORDS = {"SRCS", "SRC_DIRS", "EXCLUDE_SRCS", "INCLUDE_DIRS", "PRIV_INCLUDE_DIRS", "REQUIRES",
                  "PRIV_REQUIRES", "LDFRAGMENTS", "REQUIRED_IDF_TARGETS", "EMBED_FILES", "EMBED_TXTFILES",
                  "KCONFIG", "KCONFIG_PROJBUILD", "WHOLE_ARCHIVE"}


def _cmake_requires(text):
    """Components an ESP-IDF CMakeLists.txt requires (REQUIRES and
    PRIV_REQUIRES of idf_component_register, ${variables} of set() expanded),
    and its `set(optional ...)` list."""
    variables = {}
    for name, body in CMAKE_SET.findall(text):
        # The first definition is the one the ESP-IDF branch sees; later ones
        # belong to the host branch (a test's subset of optional modules).
        variables.setdefault(name, body.split())

    def expand(token):
        found = re.fullmatch(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}", token)
        return [item for inner in variables.get(found.group(1), []) for item in expand(inner)] if found else [token]

    requires = set()
    for body in CMAKE_REGISTER.findall(text):
        taking = False
        for token in body.split():
            if token in CMAKE_KEYWORDS:
                taking = token in ("REQUIRES", "PRIV_REQUIRES")
            elif taking:
                requires.update(item for item in expand(token) if not item.startswith("$"))
    return requires, set(variables.get("optional", []))


def requires_mismatch(root=ROOT):
    """Manifest dependencies that a unit's CMakeLists.txt does not require.

    Under ESP-IDF a component outside REQUIRES contributes no include path, so
    every unit a manifest depends on must be required by the component; an
    application's optional modules must be exactly its `set(optional ...)`,
    which its CMakeLists.txt requires when the product selected them.
    Returns [(CMakeLists path, problem)]."""
    root = Path(root)
    units = catalog(root)
    problems = []
    for unit in list(units["module"].values()) + list(units["application"].values()):
        cmake = unit.path / "CMakeLists.txt"
        if not cmake.is_file():
            continue
        requires, optional = _cmake_requires(cmake.read_text(encoding="utf-8"))
        where = cmake.relative_to(root).as_posix()
        for name in unit.depends:
            dependency = units["module"].get(name)
            if dependency is not None and dependency.component not in requires:
                problems.append((where, f"manifest needs {name} ({dependency.component}), REQUIRES lacks it"))
        if unit.kind == "application":
            wanted = {units["module"][name].component for name in unit.optional if name in units["module"]}
            if wanted != optional:
                problems.append((where, f"optional components {sorted(optional)} differ from the manifest's "
                                        f"optional modules {sorted(wanted)}"))
    return problems


def _read_board(path):
    try:
        board = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise SelectionError(f"{path}: {error}") from error
    if not isinstance(board, dict) or type(board.get("schema")) is not int or board["schema"] != 3 or "parts" not in board:
        raise SelectionError(f"{path}: expected a schema 3 board manifest with parts")
    return board


def plan(root, product, board=None):
    """Everything the build needs to know about a product.

    Without an explicit board fixture, load the product's real board.json
    and require its hardware facts before resolving any selection."""
    root = Path(root)
    _product_names(product, "selection")
    if board is None:
        manifest = root / "boards" / product["board"] / "board.json"
        if not manifest.resolve().is_relative_to(root.resolve()):
            raise SelectionError(f"board of product {product['name']}: manifest is outside the repository")
        if not manifest.is_file():
            raise SelectionError(f"board of product {product['name']}: missing manifest {manifest}")
        board = _read_board(manifest)
    needs = None
    if "parts" in board:
        check_board(board)
        needs = board_needs(board)
    units = catalog(root)
    label = f"product {product['name']}"
    any_need = _product_needs(product, "needs_any", label)
    all_needs = _product_needs(product, "needs_all", label)
    if needs is None and (any_need or all_needs):
        field = "needs_any" if any_need else "needs_all"
        raise SelectionError(f"{label}: {field} requires board parts")
    if any_need and needs.isdisjoint(any_need):
        raise SelectionError(f"{label} needs one of {sorted(any_need)}, which this board lacks")
    extra = list(product.get("modules", []))
    apps, modules, _, selected = resolve(units, product.get("applications", []), extra, needs=needs)
    if all_needs:
        missing = set(all_needs) - needs
        if missing:
            raise SelectionError(f"{label} needs {sorted(missing)}, which this board lacks")
    drivers = [name for name in network_drivers(units, modules + apps, board) if name not in selected]
    if drivers:
        apps, modules, _, selected = resolve(units, product.get("applications", []), extra + drivers,
                                             needs=needs)
    check_themes(modules, board)
    check_theme_parameters(modules + apps, product, board)
    _startup_error_guide(modules, product, board)
    check_absent_with(modules + apps, product)
    activities = activity_defaults(modules + apps)
    named = list(product.get("priorities", {})) + [name for rule in product.get("preemptions", [])
                                                    for name in (rule["activity"], rule["over"])]
    for name in named:
        if name not in activities:
            raise SelectionError(f"product {product['name']}: activity {name} is begun by no selected unit "
                                 f"(declared: {sorted(activities)})")
    lines = list(board.get("idf", []))
    for line in product.get("tirtc", {}):
        if lines and line not in lines:
            raise SelectionError(f"tirtc names ESP-IDF line {line}, which board {product['board']} does not support")
    components = {name: None for name in BASE_COMPONENTS}
    for name in board.get("components", []):
        components[name] = None
    for unit in modules + apps:
        components[unit.component] = unit.path.relative_to(root).as_posix()
    for name, directory in list(components.items()):
        if directory is None:
            found = [base / name for base in (root / "sdk", root / "sdk/parts") if (base / name).is_dir()]
            if not found:
                raise SelectionError(f"component {name} (board {product['board']}) does not exist")
            components[name] = found[0].relative_to(root).as_posix()
    return {
        "product": product["name"], "board": product["board"], "target": board.get("target"),
        "applications": [app.name for app in apps], "modules": [unit.name for unit in modules],
        "selected": sorted(selected) + [f"app.{app.name}" for app in apps],
        "components": components, "needs": sorted(needs) if needs is not None else None,
        "activities": activities,
        "network_drivers": network_drivers(units, modules + apps, board),
        "commands": vocabulary(modules + apps),
        "options": _options(root, product, components, modules + apps, board),
        "sdkconfig": _sdkconfig(product, modules + apps),
        "budget": dict(product.get("budget", {})),
        "tirtc": dict(product.get("tirtc", {})),
        "units": (apps, modules, board),
    }


def write(root, product, out_dir, board=None):
    """Writes the generated files of a product into out_dir and returns the plan."""
    result = plan(root, product, board)
    apps, modules, board = result.pop("units")
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    files = {
        "xiaotai_selected.c": generate(apps, modules, product, board),
        "xiaotai_product.h": generate_header(product, result["needs"]),
        "selection.cmake": "".join([
            "# Generated by scripts/sdk_selection.py. Do not edit.\n",
            f'set(XIAOTAI_PRODUCT_NAME "{result["product"]}")\n',
            f'set(XIAOTAI_BOARD "{result["board"]}")\n',
            f'set(XIAOTAI_SELECTED "{";".join(result["selected"])}")\n',
            f'set(XIAOTAI_SELECTED_COMPONENTS "{";".join(result["components"])}")\n',
            f'set(XIAOTAI_SELECTED_COMPONENT_DIRS "{";".join(result["components"].values())}")\n',
        ]),
        "sdkconfig.selection": "".join(line + "\n" for line in result["options"] + result["sdkconfig"]),
        # The product's AT vocabulary (spec xiaotai_at.md 4.0): what the bench
        # tool's dialect, the console script's permissions and the public
        # command list are generated from.
        "commands.json": json.dumps({"product": result["product"], "commands": result["commands"]},
                                    ensure_ascii=False, indent=1) + "\n",
    }
    for name, text in files.items():
        _write(out_dir / name, text)
    return result


def _write(path, text):
    # Repository files and generated sources use LF on every host.
    with open(path, "w", encoding="utf-8", newline="\n") as file:
        file.write(text)


def board_header(path):
    """Board-only constants. The board component does not include product glue."""
    board = _read_board(path)
    check_board(board)
    identity = board.get("identity")
    if not isinstance(identity, dict):
        raise SelectionError(f"{path}: missing board identity")
    lines = ["// Generated by scripts/sdk_selection.py. Do not edit.\n", "#pragma once\n"]
    for key in ("model", "revision", "profile_model", "profile_chip"):
        value = identity.get(key)
        if key in ("profile_model", "profile_chip") and value is None:
            literal = "nullptr"
        elif isinstance(value, str) and value and "\0" not in value:
            literal = _c_string(value)
        else:
            raise SelectionError(f"{path}: identity.{key} needs a nonempty string")
        lines.append(f"#define XIAOTAI_BOARD_{key.upper()} {literal}\n")
    lines.append(f"#define XIAOTAI_BOARD_MANIFEST_NEEDS 0x{needs_mask(board_needs(board)):x}u\n")
    return "".join(lines)


def check_needs_header(root=ROOT):
    """The NEEDS table equals xiaotai/needs.h; returns the differences."""
    header = (Path(root) / "sdk/xiaotai_port/include/xiaotai/needs.h").read_text(encoding="utf-8")
    declared = {name.lower(): 1 << int(bit)
                for name, bit in re.findall(r"XIAOTAI_NEED_([A-Z_]+)\s*=\s*1u\s*<<\s*([0-9]+)", header)}
    return sorted(set(declared.items()) ^ set(NEEDS.items()))


def main(argv=None):
    import argparse
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--apps", nargs="*", default=[])
    parser.add_argument("--modules", nargs="*", default=[])
    parser.add_argument("--capabilities", nargs="*")
    parser.add_argument("--out", type=Path, help="write the generated C file here")
    parser.add_argument("--product", type=Path, help="a product.json: resolve it against its board package")
    parser.add_argument("--variant", help="with --product: one of the product's declared variants")
    parser.add_argument("--out-dir", type=Path, help="with --product: write the generated files here")
    parser.add_argument("--board-header", type=Path, metavar="BOARD_JSON",
                        help="with --out: generate the board component's private C++ constants")
    parser.add_argument("--check", action="store_true",
                        help="check the need table against xiaotai/needs.h, every manifest, that every "
                             "configure function and activity of a public header is in its manifest, and "
                             "that manifests and CMake REQUIRES agree")
    parser.add_argument("--strict", action="store_true",
                        help="with --check: an unlisted configure function or activity, or a REQUIRES "
                             "mismatch, is an error, not a notice")
    args = parser.parse_args(argv)
    try:
        if args.board_header:
            if not args.out or args.product or args.check:
                raise SelectionError("--board-header needs --out, without --product or --check")
            _write(args.out, board_header(args.board_header))
            return 0
        if args.check:
            differences = check_needs_header()
            if differences:
                raise SelectionError(f"need table differs from xiaotai/needs.h: {differences}")
            catalog()
            missing = missing_configure()
            for path, function in missing:
                print(f"selection: {path}: {function} is not in the manifest's \"configure\"",
                      file=sys.stderr)
            mismatched = requires_mismatch()
            for path, problem in mismatched:
                print(f"selection: {path}: {problem}", file=sys.stderr)
            undeclared = missing_activities()
            for path, activity in undeclared:
                print(f"selection: {path}: activity {activity} is not in the manifest's \"activities\"",
                      file=sys.stderr)
            if (missing or mismatched or undeclared) and args.strict:
                raise SelectionError(f"{len(missing)} configure functions not listed in their manifests, "
                                     f"{len(mismatched)} CMake REQUIRES differ from manifests, "
                                     f"{len(undeclared)} activities not declared")
            notes = [f"{len(missing)} configure functions not yet listed" if missing else "",
                     f"{len(mismatched)} CMake REQUIRES differ from manifests" if mismatched else "",
                     f"{len(undeclared)} activities not yet declared" if undeclared else ""]
            print("selection: need table and manifests are consistent"
                  + "".join(f"; {note}" for note in notes if note))
            return 0
        if args.product:
            product = load_product(args.product)
            if args.variant:
                product = apply_variant(product, args.variant)
            result = write(ROOT, product, args.out_dir) if args.out_dir else plan(ROOT, product)
            result.pop("units", None)
            print(json.dumps(result))
            return 0
        apps, modules, components, _ = resolve(catalog(), args.apps, args.modules, args.capabilities)
    except SelectionError as error:
        print(f"selection: {error}", file=sys.stderr)
        return 2
    if args.out:
        _write(args.out, generate(apps, modules))
    print(json.dumps({"components": components, "applications": [a.name for a in apps],
                      "modules": [m.name for m in modules]}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
