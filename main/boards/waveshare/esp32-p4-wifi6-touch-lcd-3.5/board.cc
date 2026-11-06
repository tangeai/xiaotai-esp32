#include "selected_board.h"
namespace xiaotai::board {
Board& SelectedBoard()
{
    static SelectedIdentity board({"waveshare-p4-touch-lcd-3.5", "rev3.2"});
    return board;
}
}
