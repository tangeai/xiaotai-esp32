// SPDX-License-Identifier: MIT
#include "xiaotai/board.hpp"

namespace {
xiaotai_output_t* outputs;
size_t count;

class FeedbackBoard final : public xiaotai::board::Board {
public:
    xiaotai_board_identity_t Identity() const override { return {"feedback_fixture", "1", nullptr, nullptr}; }
    const xiaotai_output_t* Output(size_t index) override { return index < count ? &outputs[index] : nullptr; }
};
}

namespace xiaotai::board {
Board& SelectedBoard()
{
    static FeedbackBoard board;
    return board;
}
}

extern "C" void feedback_fixture_outputs(xiaotai_output_t* parts, size_t size)
{
    outputs = parts;
    count = size;
}
