#include "selected_board.h"
namespace xiaotai::board {
Board& SelectedBoard()
{
    static SelectedIdentity board({"m5stack-cores3-lite", "Lite"});
    return board;
}
}
