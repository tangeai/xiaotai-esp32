#include "selected_board.h"
namespace xiaotai::board {
Board& SelectedBoard()
{
    static SelectedIdentity board({"xmini-c3", "V3"});
    return board;
}
}
