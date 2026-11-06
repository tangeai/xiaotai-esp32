#include "selected_board.h"
namespace xiaotai::board {
Board& SelectedBoard()
{
    static SelectedIdentity board({"wt99p461-s1", "1V1"});
    return board;
}
}
