// SPDX-License-Identifier: MIT
// Replaces legacy: tests/test_hardware_boundaries.py
// Replaces legacy: tests/test_product_contracts.py
// The board's parts through the C view after a successful bring-up: nothing
// is visible and nothing of the board is called before xiaotai_hal_init(); the
// board's Init() runs once and its result is remembered; diagnostics are
// forwarded only afterwards; enumeration, roles, derived needs (kind by kind
// against needs.json) and output ownership. The failed bring-up is
// hal_init_failure.
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>
#include "needs_rules.h"
#include "xiaotai/board.hpp"
#include "xiaotai/port/mem.h"
#include "xiaotai_mem_host.h"

// Audio encodings are the codec vocabulary itself: a part's format reaches the
// media engine and links without conversion, and the former names keep their
// meaning under the new numbering (PCM16 moved from 0 to 5).
static_assert(std::is_same<xiaotai_audio_encoding_t, xiaotai_codec_t>::value, "encoding is the codec type");
static_assert(XIAOTAI_AUDIO_PCM16 == XIAOTAI_CODEC_PCM16 && XIAOTAI_CODEC_PCM16 == 5, "PCM16");
static_assert(XIAOTAI_AUDIO_OPUS == XIAOTAI_CODEC_OPUS && XIAOTAI_AUDIO_G711A == XIAOTAI_CODEC_G711A, "encoded");
static_assert(XIAOTAI_CODEC_IS_AUDIO(XIAOTAI_AUDIO_PCM16) && XIAOTAI_CODEC_IS_AUDIO(XIAOTAI_AUDIO_OPUS) &&
                  XIAOTAI_CODEC_IS_AUDIO(XIAOTAI_AUDIO_G711A),
              "every former audio name is an audio codec");
// The logical display rectangle is aliased by the UI and video headers, which
// rely on this member order.
static_assert(offsetof(xiaotai_display_area_t, x) == 0 && offsetof(xiaotai_display_area_t, y) == 2 &&
                  offsetof(xiaotai_display_area_t, width) == 4 && offsetof(xiaotai_display_area_t, height) == 6 &&
                  sizeof(xiaotai_display_area_t) == 8,
              "display area layout");

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                                     \
        }                                                                                 \
    } while (0)

namespace {

uint32_t poll_none(void*) { return 0; }
uint32_t poll_click(void*) { return XIAOTAI_INPUT_CLICK; }

// Fixture parts carry descriptors only; functions the tests call are set below.
xiaotai_audio_input_t Mics(xiaotai_codec_t encoding = XIAOTAI_CODEC_PCM16)
{
    xiaotai_audio_input_t part{};
    part.desc.encoding = encoding;
    part.desc.rate_hz = 16000;
    part.desc.channels = 4;
    part.desc.roles[0] = XIAOTAI_CHANNEL_MIC;
    part.desc.roles[1] = XIAOTAI_CHANNEL_REFERENCE;
    part.desc.roles[2] = XIAOTAI_CHANNEL_MIC;
    part.info.echo_coupling_db = XIAOTAI_AUDIO_COUPLING_UNKNOWN;
    part.info.group = 1;
    return part;
}

xiaotai_audio_output_t Speaker()
{
    xiaotai_audio_output_t part{};
    part.desc.encoding = XIAOTAI_CODEC_PCM16;
    part.desc.rate_hz = 16000;
    part.desc.channels = 1;
    part.desc.roles[0] = XIAOTAI_CHANNEL_MONO;
    part.info.group = 1;
    part.info.flags = XIAOTAI_AUDIO_OUTPUT_PACED;
    return part;
}

xiaotai_display_t Panel(bool touch = true)
{
    xiaotai_display_t part{};
    part.desc.width = 320;
    part.desc.height = 240;
    part.desc.pixel = XIAOTAI_PIXEL_RGB565;
    part.desc.touch = touch;
    part.desc.render = XIAOTAI_RENDER_PARTIAL;
    part.desc.transfer_pixels = 3200;
    return part;
}

xiaotai_input_t Key(const char* role, uint32_t (*poll)(void*), uint32_t kind = XIAOTAI_INPUT_BUTTON)
{
    xiaotai_input_t part{};
    part.desc.kind = static_cast<xiaotai_input_kind_t>(kind);
    part.desc.role = role;
    part.desc.events = XIAOTAI_INPUT_CLICK;
    part.poll = poll;
    return part;
}

xiaotai_output_t Output(uint32_t kind, const char* role, uint16_t pixels)
{
    xiaotai_output_t part{};
    part.desc.kind = static_cast<xiaotai_output_kind_t>(kind);
    part.desc.role = role;
    part.desc.pixels = pixels;
    return part;
}

xiaotai_network_t Wifi()
{
    xiaotai_network_t part{};
    part.desc.kind = XIAOTAI_NETWORK_WIFI;
    part.desc.provisioning = XIAOTAI_PROVISION_PORTAL | XIAOTAI_PROVISION_SERIAL;
    return part;
}

xiaotai_camera_t Camera()
{
    xiaotai_camera_t part{};
    part.desc.formats = XIAOTAI_CAMERA_JPEG;
    part.desc.max_width = 640;
    part.desc.max_height = 480;
    return part;
}

const xiaotai_audio_input_t kMics = Mics();
const xiaotai_audio_output_t kSpeaker = Speaker();
const xiaotai_display_t kPanel = Panel();
// A contact and a vibrator yield needs of their own: a door contact is not a
// key, a vibrator not a light. A slider yields none.
const xiaotai_input_t kKeys[] = {Key("main", poll_click), Key("volume_up", poll_none),
                                 Key("door_contact", poll_none, XIAOTAI_INPUT_CONTACT),
                                 Key("dimmer", poll_none, XIAOTAI_INPUT_SLIDER)};
const xiaotai_output_t kOutputs[] = {
    Output(XIAOTAI_OUTPUT_RGB_LIGHT, "status", 7),
    Output(XIAOTAI_OUTPUT_RELAY, "door", 1),
    Output(XIAOTAI_OUTPUT_VIBRATOR, "haptic", 1),
    Output(XIAOTAI_OUTPUT_RELAY, "door.gate", 1),
    Output(XIAOTAI_OUTPUT_RELAY, "doorbell_chime", 1),
};
const xiaotai_network_t kWifi = Wifi();
const xiaotai_measurement_t kThermometer = {{XIAOTAI_MEASURE_TEMPERATURE, "room", 1000}, nullptr, nullptr};

// The parts the board reports, one list per class. The full board is the
// default; the derivation cases swap in boards of one part.
template <typename Part>
struct List {
    const Part* parts = nullptr;
    size_t count = 0;
    const Part* At(size_t i) const { return i < count ? &parts[i] : nullptr; }
};

struct Parts {
    List<xiaotai_audio_input_t> audio_inputs;
    List<xiaotai_audio_output_t> audio_outputs;
    List<xiaotai_display_t> displays;
    List<xiaotai_input_t> inputs;
    List<xiaotai_output_t> outputs;
    List<xiaotai_camera_t> cameras;
    List<xiaotai_network_t> networks;
    List<xiaotai_measurement_t> measurements;
};

Parts FullBoard()
{
    Parts parts;
    parts.audio_inputs = {&kMics, 1};
    parts.audio_outputs = {&kSpeaker, 1};
    parts.displays = {&kPanel, 1};
    parts.inputs = {kKeys, sizeof(kKeys) / sizeof(kKeys[0])};
    parts.outputs = {kOutputs, sizeof(kOutputs) / sizeof(kOutputs[0])};
    parts.networks = {&kWifi, 1};
    parts.measurements = {&kThermometer, 1};
    return parts;
}

Parts g_parts = FullBoard();

// What the board saw: how often Init(), the getters and Report() ran, and
// what the C view answered while Init() was still running.
int g_init_calls = 0;
int g_getter_calls = 0;
int g_report_calls = 0;
xiaotai_err_t g_nested_init = XIAOTAI_OK;
const xiaotai_display_t* g_display_during_init = &kPanel;
uint32_t g_needs_during_init = ~0u;

class FixtureBoard final : public xiaotai::board::Board {
public:
    xiaotai_err_t Init() override
    {
        ++g_init_calls;
        // Bring-up is not finished: the C view shows no part yet, and a call
        // back into xiaotai_hal_init() neither recurses nor reports success.
        g_display_during_init = xiaotai_hal_display(0);
        g_needs_during_init = xiaotai_hal_needs();
        g_nested_init = xiaotai_hal_init();
        return XIAOTAI_OK;
    }
    xiaotai_board_identity_t Identity() const override { return {"fixture", "1", "Fixture-1", "Chip-1"}; }
    const xiaotai_audio_input_t* AudioInput(size_t i) override { return Count(g_parts.audio_inputs.At(i)); }
    const xiaotai_audio_output_t* AudioOutput(size_t i) override { return Count(g_parts.audio_outputs.At(i)); }
    const xiaotai_display_t* Display(size_t i) override { return Count(g_parts.displays.At(i)); }
    const xiaotai_input_t* Input(size_t i) override { return Count(g_parts.inputs.At(i)); }
    const xiaotai_output_t* Output(size_t i) override { return Count(g_parts.outputs.At(i)); }
    const xiaotai_camera_t* Camera(size_t i) override { return Count(g_parts.cameras.At(i)); }
    const xiaotai_network_t* Network(size_t i) override { return Count(g_parts.networks.At(i)); }
    const xiaotai_measurement_t* Measurement(size_t i) override { return Count(g_parts.measurements.At(i)); }
    void Report(xiaotai_hal_write_fn_t write, void* ctx) override
    {
        ++g_report_calls;
        write(ctx, "pmu.reg90=0x0f");
        write(ctx, "i2s.din.high=512 i2s.din.edges=40");
    }

private:
    template <typename Part>
    static const Part* Count(const Part* part)
    {
        ++g_getter_calls;
        return part;
    }
};

struct Lines {
    std::vector<std::string> lines;
};

void Collect(void* ctx, const char* line) { static_cast<Lines*>(ctx)->lines.emplace_back(line); }

uint32_t MemoryNeeds() { return xiaotai_mem_has_external() ? static_cast<uint32_t>(XIAOTAI_NEED_EXTERNAL_RAM) : 0u; }

// --------------------------------------------------- needs.json, rule by rule

bool InList(const char* const* list, const char* text)
{
    for (; *list; ++list) {
        if (text && std::strcmp(*list, text) == 0) return true;
    }
    return false;
}

// The needs.json role match: the family itself or the family followed by ".".
bool InFamilies(const char* const* families, const char* role)
{
    for (; role && *families; ++families) {
        const size_t length = std::strlen(*families);
        if (std::strncmp(role, *families, length) == 0 && (role[length] == '\0' || role[length] == '.')) return true;
    }
    return false;
}

// The part's fields as needs.json spells them; nullptr: the class has no such
// field (a rule on it then cannot match) or the value has no spelling.
struct Fields {
    explicit Fields(const char* part_class, const char* kind_text = nullptr, const char* role_text = nullptr)
        : part(part_class), kind(kind_text), role(role_text)
    {
    }
    const char* part;
    const char* kind;
    const char* role;
    const char* encoding = nullptr;
    int touch = -1;
};

// The needs a board holding exactly this one part satisfies, by the rules of
// needs.json, plus the memory port's answer.
uint32_t Expected(const Fields& fields)
{
    uint32_t needs = 0;
    for (const auto& rule : needs_json::kRules) {
        if (rule.memory) {
            if (std::strcmp(rule.memory, "external_ram") == 0 && xiaotai_mem_has_external()) needs |= 1u << rule.bit;
            continue;
        }
        if (!rule.part || std::strcmp(rule.part, fields.part) != 0) continue;
        if (rule.kind && !InList(rule.kind, fields.kind)) continue;
        if (rule.encoding && !InList(rule.encoding, fields.encoding)) continue;
        if (rule.role && !InFamilies(rule.role, fields.role)) continue;
        if (rule.touch >= 0 && rule.touch != fields.touch) continue;
        needs |= 1u << rule.bit;
    }
    return needs;
}

const char* Spell(const needs_json::Spelling* vocabulary, uint32_t value)
{
    for (; vocabulary->text; ++vocabulary) {
        if (vocabulary->value == value) return vocabulary->text;
    }
    return nullptr;
}

// Every kind and encoding the vocabulary spells, alone on a board, derives
// exactly what needs.json says; so do roles in and out of a role family and
// the classes whose mere presence is a need.
int CheckNeedsAgainstJson()
{
    const char* const roles[] = {"main", "status", "door", "door.gate", "doorbell_chime", "doors", "gate.door", nullptr};
    const size_t role_count = sizeof(roles) / sizeof(roles[0]);
    int cases = 0;

    for (const auto* kind = needs_json::kinput_kind; kind->text; ++kind) {
        for (size_t r = 0; r < role_count; ++r) {
            const xiaotai_input_t part = Key(roles[r], poll_none, kind->value);
            g_parts = Parts{};
            g_parts.inputs = {&part, 1};
            const uint32_t expected = Expected(Fields("input", kind->text, roles[r]));
            if (xiaotai_hal_needs() != expected) {
                std::fprintf(stderr, "input kind %s role %s: needs 0x%x, needs.json 0x%x\n", kind->text,
                             roles[r] ? roles[r] : "(none)", xiaotai_hal_needs(), expected);
                return 1;
            }
            ++cases;
        }
    }
    for (const auto* kind = needs_json::koutput_kind; kind->text; ++kind) {
        for (size_t r = 0; r < role_count; ++r) {
            const xiaotai_output_t part = Output(kind->value, roles[r], 1);
            g_parts = Parts{};
            g_parts.outputs = {&part, 1};
            const uint32_t expected = Expected(Fields("output", kind->text, roles[r]));
            if (xiaotai_hal_needs() != expected) {
                std::fprintf(stderr, "output kind %s role %s: needs 0x%x, needs.json 0x%x\n", kind->text,
                             roles[r] ? roles[r] : "(none)", xiaotai_hal_needs(), expected);
                return 1;
            }
            ++cases;
        }
    }
    // Encodings: every spelled one, and NONE (a descriptor left zero), which
    // has no spelling and so matches no encoding rule.
    std::vector<uint32_t> encodings;
    for (const auto* encoding = needs_json::kaudio_input_encoding; encoding->text; ++encoding) {
        encodings.push_back(encoding->value);
    }
    encodings.push_back(XIAOTAI_CODEC_NONE);
    for (const uint32_t encoding : encodings) {
        const xiaotai_audio_input_t part = Mics(static_cast<xiaotai_codec_t>(encoding));
        g_parts = Parts{};
        g_parts.audio_inputs = {&part, 1};
        Fields fields("audio_input");
        fields.encoding = Spell(needs_json::kaudio_input_encoding, encoding);
        if (xiaotai_hal_needs() != Expected(fields)) {
            std::fprintf(stderr, "audio input encoding %u: needs 0x%x, needs.json 0x%x\n", encoding,
                         xiaotai_hal_needs(), Expected(fields));
            return 1;
        }
        ++cases;
    }
    for (const bool touch : {false, true}) {
        const xiaotai_display_t part = Panel(touch);
        g_parts = Parts{};
        g_parts.displays = {&part, 1};
        Fields fields("display");
        fields.touch = touch ? 1 : 0;
        CHECK(xiaotai_hal_needs() == Expected(fields));
        ++cases;
    }
    const xiaotai_camera_t camera = Camera();
    g_parts = Parts{};
    g_parts.cameras = {&camera, 1};
    CHECK(xiaotai_hal_needs() == Expected(Fields("camera")));
    g_parts = Parts{};
    g_parts.audio_outputs = {&kSpeaker, 1};
    CHECK(xiaotai_hal_needs() == Expected(Fields("audio_output")));
    g_parts = Parts{};
    g_parts.networks = {&kWifi, 1};
    CHECK(xiaotai_hal_needs() == Expected(Fields("network")));
    // Measurements and the empty board give no need of their own.
    g_parts = Parts{};
    g_parts.measurements = {&kThermometer, 1};
    CHECK(xiaotai_hal_needs() == Expected(Fields("measurement")) && Expected(Fields("measurement")) == MemoryNeeds());
    g_parts = Parts{};
    CHECK(xiaotai_hal_needs() == MemoryNeeds());
    cases += 5;
    g_parts = FullBoard();

    // The generated tables are not empty, and every class a rule names is one
    // exercised above: a rule on a new class makes this test fail until it
    // builds parts of that class too.
    static const char* const kClasses[] = {"audio_input", "audio_output", "display", "input",
                                           "output",      "camera",       "network", nullptr};
    for (const auto& rule : needs_json::kRules) {
        if (rule.part && !InList(kClasses, rule.part)) {
            std::fprintf(stderr, "needs.json rule %s on class %s: not exercised by this test\n", rule.name, rule.part);
            return 1;
        }
    }
    CHECK(cases > 50);
    return 0;
}

}  // namespace

xiaotai::board::Board& xiaotai::board::SelectedBoard()
{
    static FixtureBoard board;
    return board;
}

int main()
{
    xiaotai_mem_host_clear();

    // Before the bring-up: identity is a static fact and available; every
    // part query is NULL, by index and by role; the needs hold no part; the
    // report writes nothing; and the board was asked for no part.
    xiaotai_board_identity_t identity{};
    xiaotai_hal_identity(&identity);
    CHECK(std::strcmp(identity.model, "fixture") == 0 && std::strcmp(identity.profile_chip, "Chip-1") == 0);
    CHECK(!xiaotai_hal_audio_input(0) && !xiaotai_hal_audio_output(0) && !xiaotai_hal_display(0));
    CHECK(!xiaotai_hal_input(0) && !xiaotai_hal_output(0) && !xiaotai_hal_camera(0));
    CHECK(!xiaotai_hal_network(0) && !xiaotai_hal_measurement(0));
    CHECK(!xiaotai_hal_input_by_role("main") && !xiaotai_hal_output_by_role("status"));
    CHECK(!xiaotai_hal_measurement_by_role("room"));
    CHECK(xiaotai_hal_needs() == MemoryNeeds());
    Lines before;
    xiaotai_hal_report(Collect, &before);
    CHECK(before.lines.empty());
    CHECK(g_init_calls == 0 && g_getter_calls == 0 && g_report_calls == 0);

    // The bring-up: Init() runs once. While it runs the C view still shows
    // nothing and a nested xiaotai_hal_init() is refused without recursing.
    CHECK(xiaotai_hal_init() == XIAOTAI_OK);
    CHECK(g_init_calls == 1);
    CHECK(g_display_during_init == nullptr && g_needs_during_init == MemoryNeeds());
    CHECK(g_nested_init == XIAOTAI_ERR_INVALID_STATE);
    CHECK(g_getter_calls == 0);
    // Remembered: further calls return the result without running Init().
    CHECK(xiaotai_hal_init() == XIAOTAI_OK && xiaotai_hal_init() == XIAOTAI_OK);
    CHECK(g_init_calls == 1);

    // Diagnostics reach the board now, line by line, in its order and with the
    // caller's context; without a sink the board is not called.
    Lines report;
    xiaotai_hal_report(Collect, &report);
    CHECK(g_report_calls == 1);
    CHECK(report.lines == (std::vector<std::string>{"pmu.reg90=0x0f", "i2s.din.high=512 i2s.din.edges=40"}));
    xiaotai_hal_report(nullptr, &report);
    CHECK(g_report_calls == 1 && report.lines.size() == 2);

    // Parts by index and role.
    xiaotai_hal_identity(&identity);
    CHECK(std::strcmp(identity.model, "fixture") == 0 && std::strcmp(identity.profile_model, "Fixture-1") == 0);
    CHECK(xiaotai_hal_audio_input(0) == &kMics && !xiaotai_hal_audio_input(1));
    CHECK(xiaotai_hal_audio_output(0) == &kSpeaker && xiaotai_hal_display(0) == &kPanel && !xiaotai_hal_camera(0));
    CHECK(xiaotai_hal_network(0) == &kWifi && !xiaotai_hal_network(1));
    CHECK(xiaotai_hal_input_by_role("volume_up") == &kKeys[1] && !xiaotai_hal_input_by_role("doorbell"));
    CHECK(xiaotai_hal_input_by_role("door_contact") == &kKeys[2] && !xiaotai_hal_input(4));
    CHECK(xiaotai_hal_output_by_role("door") == &kOutputs[1] && !xiaotai_hal_output(5));
    CHECK(xiaotai_hal_measurement_by_role("room") == &kThermometer && !xiaotai_hal_measurement(1));
    CHECK(!xiaotai_hal_measurement_by_role("battery") && !xiaotai_hal_input_by_role(nullptr));
    CHECK(xiaotai_hal_input_by_role("main")->poll(nullptr) == XIAOTAI_INPUT_CLICK);
    CHECK(g_getter_calls > 0);

    const uint32_t expected = XIAOTAI_NEED_AUDIO_IN | XIAOTAI_NEED_PCM_IN | XIAOTAI_NEED_AUDIO_OUT |
                              XIAOTAI_NEED_DISPLAY | XIAOTAI_NEED_TOUCH | XIAOTAI_NEED_KEY |
                              XIAOTAI_NEED_LIGHT | XIAOTAI_NEED_RELAY | XIAOTAI_NEED_NETWORK |
                              XIAOTAI_NEED_CONTACT | XIAOTAI_NEED_VIBRATOR | XIAOTAI_NEED_DOOR_LOCK;
    // External RAM comes from the memory port; the host port reports none.
    CHECK(!xiaotai_mem_has_external());
    CHECK(xiaotai_hal_needs() == expected);

    // door_lock is a relay whose role is in the family "door" (needs.json):
    // the role itself or "door.<name>". Any other relay gives only RELAY, and
    // a part of another kind gives nothing for its door role.
    struct DoorCase {
        xiaotai_output_t part;
        uint32_t needs;
    };
    const DoorCase door_cases[] = {
        {Output(XIAOTAI_OUTPUT_RELAY, "door", 1), XIAOTAI_NEED_RELAY | XIAOTAI_NEED_DOOR_LOCK},
        {Output(XIAOTAI_OUTPUT_RELAY, "door.gate", 1), XIAOTAI_NEED_RELAY | XIAOTAI_NEED_DOOR_LOCK},
        {Output(XIAOTAI_OUTPUT_RELAY, "doorbell_chime", 1), XIAOTAI_NEED_RELAY},
        {Output(XIAOTAI_OUTPUT_RELAY, "doors", 1), XIAOTAI_NEED_RELAY},
        {Output(XIAOTAI_OUTPUT_RELAY, "gate.door", 1), XIAOTAI_NEED_RELAY},
        {Output(XIAOTAI_OUTPUT_RELAY, nullptr, 1), XIAOTAI_NEED_RELAY},
        {Output(XIAOTAI_OUTPUT_LIGHT, "door", 1), XIAOTAI_NEED_LIGHT},
        {Output(XIAOTAI_OUTPUT_VIBRATOR, "door.gate", 1), XIAOTAI_NEED_VIBRATOR},
    };
    const uint32_t without_outputs = expected & ~static_cast<uint32_t>(XIAOTAI_NEED_LIGHT | XIAOTAI_NEED_RELAY |
                                                                        XIAOTAI_NEED_VIBRATOR | XIAOTAI_NEED_DOOR_LOCK);
    for (const DoorCase& c : door_cases) {
        g_parts.outputs = {&c.part, 1};
        CHECK(xiaotai_hal_needs() == (without_outputs | c.needs));
    }
    g_parts = FullBoard();
    CHECK(xiaotai_hal_needs() == expected);

    // Only PCM16 capture is PCM. Encoded capture (Opus over a serial voice
    // chip, G.711) still is audio input; a descriptor left zero names
    // XIAOTAI_CODEC_NONE since PCM16 is no longer 0, and gives no PCM need.
    const xiaotai_codec_t not_pcm[] = {XIAOTAI_CODEC_OPUS, XIAOTAI_CODEC_G711A, XIAOTAI_CODEC_G711U,
                                       XIAOTAI_CODEC_NONE};
    for (const xiaotai_codec_t encoding : not_pcm) {
        const xiaotai_audio_input_t mics = Mics(encoding);
        g_parts.audio_inputs = {&mics, 1};
        CHECK(xiaotai_hal_needs() == (expected & ~static_cast<uint32_t>(XIAOTAI_NEED_PCM_IN)));
    }
    g_parts = FullBoard();
    CHECK(xiaotai_hal_needs() == expected);

    // Every input and output kind, every capture encoding, touch and the
    // presence classes, each alone on the board, against needs.json itself.
    if (CheckNeedsAgainstJson() != 0) return 1;

    // Ownership: a claimed family covers "door" and "door.<name>", not a role
    // that merely starts with the same letters; the rest stays unclaimed.
    static xiaotai_output_claim_t lock{"door", "lock", nullptr};
    static xiaotai_output_claim_t again{"door", "lock", nullptr};
    static xiaotai_output_claim_t rival{"door", "gate", nullptr};
    static xiaotai_output_claim_t dotted{"door.gate", "gate", nullptr};
    static xiaotai_output_claim_t nameless{"siren", "", nullptr};
    CHECK(!xiaotai_hal_output_owner(&kOutputs[1]));
    CHECK(xiaotai_hal_output_claim(&lock) == XIAOTAI_OK);
    CHECK(xiaotai_hal_output_claim(&again) == XIAOTAI_OK);
    CHECK(xiaotai_hal_output_claim(&rival) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_hal_output_claim(&dotted) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_hal_output_claim(&nameless) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_hal_output_claim(nullptr) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(std::strcmp(xiaotai_hal_output_owner(&kOutputs[1]), "lock") == 0);
    CHECK(std::strcmp(xiaotai_hal_output_owner(&kOutputs[3]), "lock") == 0);
    CHECK(!xiaotai_hal_output_owner(&kOutputs[4]) && !xiaotai_hal_output_owner(&kOutputs[0]));
    CHECK(!xiaotai_hal_output_owner(nullptr));

    // The hardware abstraction allocates nothing of its own: bring-up,
    // queries, needs and report went through no port allocator.
    CHECK(xiaotai_mem_host_allocations() == 0);
    CHECK(g_init_calls == 1);
    std::printf("PASS: nothing before bring-up, Init once and remembered, report after success only, parts by "
                "index and role, needs per kind against needs.json (PCM only from PCM16, door_lock by role "
                "family), output ownership\n");
    return 0;
}
