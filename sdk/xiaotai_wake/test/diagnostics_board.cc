// SPDX-License-Identifier: MIT
#include "xiaotai/board.hpp"

namespace xiaotai::board {
namespace {
class TestBoard final : public Board {
public:
    xiaotai_board_identity_t Identity() const override { return {}; }
};
}  // namespace

Board& SelectedBoard()
{
    static TestBoard board;
    return board;
}
}  // namespace xiaotai::board
