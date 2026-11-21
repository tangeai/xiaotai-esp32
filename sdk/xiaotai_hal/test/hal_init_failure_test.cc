// SPDX-License-Identifier: MIT
// A board whose bring-up fails: the failure is the result of this boot. Later
// calls return the same error without running the board's Init() again, the
// board's parts never become visible (its getters are never called), its
// diagnostics are never asked for, and the needs hold no part; the board can
// still be named. A process runs xiaotai_hal_init() once, so this is a test
// of its own beside hal_board.
#include <cstdio>
#include <cstring>
#include "xiaotai/board.hpp"
#include "xiaotai/port/mem.h"
#include "xiaotai_mem_host.h"

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                                     \
        }                                                                                 \
    } while (0)

namespace {

// The board's own failure (an expander that did not answer, say); any error
// but INVALID_STATE, so it cannot be mistaken for the nested-call refusal.
constexpr xiaotai_err_t kBoardError = XIAOTAI_ERR_TIMEOUT;

// Parts the board would report if it were asked: a gated query that leaked
// through would return them.
const xiaotai_output_t kLight = {{XIAOTAI_OUTPUT_LIGHT, "status", 1}, nullptr, nullptr, nullptr};
const xiaotai_input_t kKey = {{XIAOTAI_INPUT_BUTTON, "main", XIAOTAI_INPUT_CLICK, 0, 0}, nullptr, nullptr,
                              nullptr, nullptr, nullptr};
const xiaotai_measurement_t kBattery = {{XIAOTAI_MEASURE_BATTERY_CHARGE, "battery", 0}, nullptr, nullptr};

int g_init_calls = 0;
int g_getter_calls = 0;
int g_report_calls = 0;

class FailingBoard final : public xiaotai::board::Board {
public:
    xiaotai_err_t Init() override
    {
        ++g_init_calls;
        return kBoardError;
    }
    xiaotai_board_identity_t Identity() const override { return {"failing", "2", nullptr, nullptr}; }
    const xiaotai_input_t* Input(size_t i) override { return Count(i == 0 ? &kKey : nullptr); }
    const xiaotai_output_t* Output(size_t i) override { return Count(i == 0 ? &kLight : nullptr); }
    const xiaotai_measurement_t* Measurement(size_t i) override { return Count(i == 0 ? &kBattery : nullptr); }
    void Report(xiaotai_hal_write_fn_t write, void* ctx) override
    {
        ++g_report_calls;
        write(ctx, "pmu.reg90=0x00");
    }

private:
    template <typename Part>
    static const Part* Count(const Part* part)
    {
        ++g_getter_calls;
        return part;
    }
};

int g_lines = 0;
void CountLine(void* ctx, const char*) { ++*static_cast<int*>(ctx); }

}  // namespace

xiaotai::board::Board& xiaotai::board::SelectedBoard()
{
    static FailingBoard board;
    return board;
}

int main()
{
    xiaotai_mem_host_clear();
    const uint32_t memory_needs = xiaotai_mem_has_external() ? static_cast<uint32_t>(XIAOTAI_NEED_EXTERNAL_RAM) : 0u;

    CHECK(xiaotai_hal_init() == kBoardError);
    CHECK(g_init_calls == 1);
    // Final for this boot: the same error, the board's work not repeated.
    for (int again = 0; again < 3; ++again) CHECK(xiaotai_hal_init() == kBoardError);
    CHECK(g_init_calls == 1);

    // No part becomes visible, by index or by role, and the board's getters
    // are never called after its failure.
    CHECK(!xiaotai_hal_input(0) && !xiaotai_hal_output(0) && !xiaotai_hal_measurement(0));
    CHECK(!xiaotai_hal_audio_input(0) && !xiaotai_hal_audio_output(0) && !xiaotai_hal_display(0));
    CHECK(!xiaotai_hal_camera(0) && !xiaotai_hal_network(0));
    CHECK(!xiaotai_hal_input_by_role("main") && !xiaotai_hal_output_by_role("status"));
    CHECK(!xiaotai_hal_measurement_by_role("battery"));
    CHECK(xiaotai_hal_needs() == memory_needs);
    CHECK(g_getter_calls == 0);

    // Diagnostics: the board's chips were not brought up, so it is not asked.
    xiaotai_hal_report(CountLine, &g_lines);
    CHECK(g_lines == 0 && g_report_calls == 0);

    // The board can still be named, so the failed start can say which board.
    xiaotai_board_identity_t identity{};
    xiaotai_hal_identity(&identity);
    CHECK(std::strcmp(identity.model, "failing") == 0 && std::strcmp(identity.revision, "2") == 0);
    CHECK(!identity.profile_model && !identity.profile_chip);

    CHECK(xiaotai_mem_host_allocations() == 0);
    std::printf("PASS: a failed bring-up is final (same error, Init once), no part, no getter, no report\n");
    return 0;
}
