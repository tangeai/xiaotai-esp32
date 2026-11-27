// SPDX-License-Identifier: MIT
// C view of the selected board's parts, the needs they satisfy, the one-time
// board bring-up and the board's diagnostics.
#include <atomic>
#include <cstdint>
#include <cstring>
#include "xiaotai/board.hpp"
#include "xiaotai/port/mem.h"

using xiaotai::board::Board;
using xiaotai::board::SelectedBoard;

namespace {

// Bring-up state. xiaotai_hal_init() runs on the [glue] task, but other tasks
// may already ask: the console the glue opens before the board serves
// diagnostics (xiaotai_hal_report) from its own task. So the state is an
// atomic published with release after the result is stored, and every reader
// takes it with acquire: a reader that sees kReady sees the board's part
// objects complete. Only loads and stores, no read-modify-write, so CPUs
// without atomic instructions need no library support.
enum State : uint8_t {
    kNotRun = 0,
    kRunning,  // Board::Init() is executing
    kFailed,   // Board::Init() failed: final for this boot
    kReady,
};

std::atomic<uint8_t> g_state{kNotRun};
// Written once before g_state leaves kRunning; read only after an acquire load
// that saw kFailed or kReady.
xiaotai_err_t g_result = XIAOTAI_OK;

// The board once Init() succeeded, otherwise nullptr: before that no getter
// of the board is called at all (board.hpp: after a failure the getters are
// never called; before Init() the part objects do not exist).
Board* ReadyBoard()
{
    return g_state.load(std::memory_order_acquire) == kReady ? &SelectedBoard() : nullptr;
}

template <typename Part>
const Part* Query(const Part* (Board::*getter)(size_t), size_t index)
{
    Board* board = ReadyBoard();
    return board ? (board->*getter)(index) : nullptr;
}

}  // namespace

extern "C" xiaotai_err_t xiaotai_hal_init(void)
{
    switch (g_state.load(std::memory_order_acquire)) {
    case kReady:
    case kFailed:
        // Remembered: the board's work is never repeated, a failure stays the
        // result of this boot.
        return g_result;
    case kRunning:
        // Called again from inside Board::Init(): the bring-up is not finished,
        // so there is no result to give and recursing would start it twice.
        return XIAOTAI_ERR_INVALID_STATE;
    default:
        break;
    }
    g_state.store(kRunning, std::memory_order_relaxed);
    g_result = SelectedBoard().Init();
    g_state.store(g_result == XIAOTAI_OK ? kReady : kFailed, std::memory_order_release);
    return g_result;
}

extern "C" void xiaotai_hal_identity(xiaotai_board_identity_t* identity)
{
    // Identity strings are static facts of the board class, not parts: they
    // are available before and regardless of the bring-up, so a failed start
    // can still name its board.
    if (identity) *identity = SelectedBoard().Identity();
}

extern "C" const xiaotai_audio_input_t* xiaotai_hal_audio_input(size_t index)
{
    return Query(&Board::AudioInput, index);
}

extern "C" const xiaotai_audio_output_t* xiaotai_hal_audio_output(size_t index)
{
    return Query(&Board::AudioOutput, index);
}

extern "C" const xiaotai_display_t* xiaotai_hal_display(size_t index) { return Query(&Board::Display, index); }
extern "C" const xiaotai_input_t* xiaotai_hal_input(size_t index) { return Query(&Board::Input, index); }
extern "C" const xiaotai_output_t* xiaotai_hal_output(size_t index) { return Query(&Board::Output, index); }
extern "C" const xiaotai_camera_t* xiaotai_hal_camera(size_t index) { return Query(&Board::Camera, index); }
extern "C" const xiaotai_network_t* xiaotai_hal_network(size_t index) { return Query(&Board::Network, index); }
extern "C" const xiaotai_measurement_t* xiaotai_hal_measurement(size_t index)
{
    return Query(&Board::Measurement, index);
}

namespace {

template <typename Part, typename Getter>
const Part* FindByRole(Getter get, const char* role)
{
    for (size_t i = 0; role; ++i) {
        const Part* part = get(i);
        if (!part) break;
        if (part->desc.role && std::strcmp(part->desc.role, role) == 0) return part;
    }
    return nullptr;
}

}  // namespace

extern "C" const xiaotai_input_t* xiaotai_hal_input_by_role(const char* role)
{
    return FindByRole<xiaotai_input_t>(xiaotai_hal_input, role);
}

extern "C" const xiaotai_output_t* xiaotai_hal_output_by_role(const char* role)
{
    return FindByRole<xiaotai_output_t>(xiaotai_hal_output, role);
}

extern "C" const xiaotai_measurement_t* xiaotai_hal_measurement_by_role(const char* role)
{
    return FindByRole<xiaotai_measurement_t>(xiaotai_hal_measurement, role);
}

extern "C" void xiaotai_hal_report(xiaotai_hal_write_fn_t write, void* ctx)
{
    // Board::Report() reads chips the board brought up in Init(): never before
    // that succeeded (a failed board writes no line), and never without a sink.
    Board* board = ReadyBoard();
    if (board && write) board->Report(write, ctx);
}

namespace {

// Claims form a list in the callers' static storage: no capacity and no
// allocation. It is written only during the framework's init phase on the
// owner task and read afterwards (see hal.h), so it needs no lock.
xiaotai_output_claim_t* g_claims = nullptr;

bool ValidFamily(const char* family)
{
    if (!family || !*family) return false;
    for (const char* c = family; *c; ++c) {
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_')) return false;
    }
    return true;
}

// True when role is in family: equal to it, or it followed by ".".
bool InFamily(const char* role, const char* family)
{
    const size_t length = std::strlen(family);
    return std::strncmp(role, family, length) == 0 && (role[length] == '\0' || role[length] == '.');
}

}  // namespace

extern "C" xiaotai_err_t xiaotai_hal_output_claim(xiaotai_output_claim_t* claim)
{
    if (!claim || !ValidFamily(claim->family) || !claim->owner || !*claim->owner) return XIAOTAI_ERR_INVALID_ARG;
    for (const auto* held = g_claims; held; held = held->next) {
        if (std::strcmp(held->family, claim->family) != 0) continue;
        return std::strcmp(held->owner, claim->owner) == 0 ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_STATE;
    }
    claim->next = g_claims;
    g_claims = claim;
    return XIAOTAI_OK;
}

extern "C" const char* xiaotai_hal_output_owner(const xiaotai_output_t* part)
{
    if (!part || !part->desc.role) return nullptr;
    for (const auto* held = g_claims; held; held = held->next) {
        if (InFamily(part->desc.role, held->family)) return held->owner;
    }
    return nullptr;
}

namespace {

// The rules of sdk/xiaotai_hal/needs.json for the descriptor fields that are
// enumerations, one row per kind. A kind without a need maps to 0; a new kind
// is one more row here and one more entry there.
struct KindNeed {
    uint32_t kind;
    uint32_t need;
};

constexpr KindNeed kInputNeeds[] = {
    {XIAOTAI_INPUT_BUTTON, XIAOTAI_NEED_KEY},
    {XIAOTAI_INPUT_ENCODER, XIAOTAI_NEED_ENCODER},
    {XIAOTAI_INPUT_CARD, XIAOTAI_NEED_CARD},
    {XIAOTAI_INPUT_VOICE, XIAOTAI_NEED_VOICE_COMMAND},
    {XIAOTAI_INPUT_CONTACT, XIAOTAI_NEED_CONTACT},
    {XIAOTAI_INPUT_SLIDER, 0},
};

constexpr KindNeed kOutputNeeds[] = {
    {XIAOTAI_OUTPUT_LIGHT, XIAOTAI_NEED_LIGHT},
    {XIAOTAI_OUTPUT_RGB_LIGHT, XIAOTAI_NEED_LIGHT},
    {XIAOTAI_OUTPUT_VIBRATOR, XIAOTAI_NEED_VIBRATOR},
    {XIAOTAI_OUTPUT_RELAY, XIAOTAI_NEED_RELAY},
};

// The rules of needs.json whose 'where' also names a role family: the part
// must be of the kind and its role in the family (InFamily). Such a need comes
// in addition to the kind's own row above.
struct RoleNeed {
    uint32_t kind;
    const char* family;
    uint32_t need;
};

constexpr RoleNeed kOutputRoleNeeds[] = {
    {XIAOTAI_OUTPUT_RELAY, "door", XIAOTAI_NEED_DOOR_LOCK},
};

template <size_t N>
uint32_t NeedOf(const KindNeed (&table)[N], uint32_t kind)
{
    for (const auto& row : table) {
        if (row.kind == kind) return row.need;
    }
    return 0;
}

template <size_t N>
uint32_t RoleNeedOf(const RoleNeed (&table)[N], uint32_t kind, const char* role)
{
    uint32_t needs = 0;
    for (const auto& row : table) {
        if (row.kind == kind && role && InFamily(role, row.family)) needs |= row.need;
    }
    return needs;
}

}  // namespace

extern "C" uint32_t xiaotai_hal_needs(void)
{
    // Parts through the gated queries: before a successful bring-up the board
    // shows none, and only the memory port's answer remains.
    uint32_t needs = 0;
    for (size_t i = 0; const auto* input = xiaotai_hal_audio_input(i); ++i) {
        needs |= XIAOTAI_NEED_AUDIO_IN;
        // Only a part that names PCM16 is PCM: NONE (a descriptor left zero) and
        // encoded capture never stand in for it.
        if (input->desc.encoding == XIAOTAI_CODEC_PCM16) needs |= XIAOTAI_NEED_PCM_IN;
    }
    if (xiaotai_hal_audio_output(0)) needs |= XIAOTAI_NEED_AUDIO_OUT;
    for (size_t i = 0; const auto* display = xiaotai_hal_display(i); ++i) {
        needs |= XIAOTAI_NEED_DISPLAY;
        if (display->desc.touch) needs |= XIAOTAI_NEED_TOUCH;
    }
    for (size_t i = 0; const auto* input = xiaotai_hal_input(i); ++i) needs |= NeedOf(kInputNeeds, input->desc.kind);
    for (size_t i = 0; const auto* output = xiaotai_hal_output(i); ++i) {
        needs |= NeedOf(kOutputNeeds, output->desc.kind);
        needs |= RoleNeedOf(kOutputRoleNeeds, output->desc.kind, output->desc.role);
    }
    if (xiaotai_hal_camera(0)) needs |= XIAOTAI_NEED_CAMERA;
    if (xiaotai_hal_network(0)) needs |= XIAOTAI_NEED_NETWORK;
    if (xiaotai_mem_has_external()) needs |= XIAOTAI_NEED_EXTERNAL_RAM;
    return needs;
}
