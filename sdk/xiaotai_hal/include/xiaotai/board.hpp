// SPDX-License-Identifier: MIT
#pragma once
// The board class: the one thing a board author writes. It brings up what the
// board's parts share and answers which parts the board has; the parts
// themselves come from the parts library (sdk/parts) configured with this
// board's pins and facts. Everything above reaches them through xiaotai/hal.h.
#include <cstddef>
#include "xiaotai/hal.h"

namespace xiaotai::board {

class Board {
public:
    virtual ~Board() = default;

    // [board] Called once by xiaotai_hal_init(), before any getter. Brings up
    // shared buses, power rails and expanders in the order the hardware needs,
    // and creates the part objects; opens no part. A failure is final for this
    // boot: the getters are then never called.
    virtual xiaotai_err_t Init() { return XIAOTAI_OK; }

    // [any] Static strings.
    virtual xiaotai_board_identity_t Identity() const = 0;

    // [any] Part by index, nullptr past the end. Indexes are dense, and a
    // getter returns the same object for the same index for the whole run.
    // Index 0 is the part an engine uses when it needs just one of a kind.
    virtual const xiaotai_audio_input_t* AudioInput(size_t) { return nullptr; }
    virtual const xiaotai_audio_output_t* AudioOutput(size_t) { return nullptr; }
    virtual const xiaotai_display_t* Display(size_t) { return nullptr; }
    virtual const xiaotai_input_t* Input(size_t) { return nullptr; }
    virtual const xiaotai_output_t* Output(size_t) { return nullptr; }
    virtual const xiaotai_camera_t* Camera(size_t) { return nullptr; }
    virtual const xiaotai_network_t* Network(size_t) { return nullptr; }
    virtual const xiaotai_measurement_t* Measurement(size_t) { return nullptr; }

    // [any] Hardware diagnostics for xiaotai_hal_report(): the board forwards
    // to the report functions of its chips and parts. Called only after Init()
    // succeeded; reads only, and never from a real-time task.
    virtual void Report(xiaotai_hal_write_fn_t, void*) {}

protected:
    Board() = default;
    Board(const Board&) = delete;
    Board& operator=(const Board&) = delete;
};

// Exactly one board in a build defines this. It returns a function-local
// static object: no heap-allocated factory, no work in the constructor.
Board& SelectedBoard();

}  // namespace xiaotai::board
