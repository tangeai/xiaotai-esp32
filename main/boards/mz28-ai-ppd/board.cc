#include "selected_board.h"
namespace xiaotai::board {
Board& SelectedBoard()
{
    static SelectedIdentity board({"mz28-ai-ppd", "V1.2"});
    return board;
}
}
