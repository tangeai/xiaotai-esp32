#include "application.h"
#include "selected_board.h"
#include <cassert>

static int calls;
static esp_err_t result;
extern "C" esp_err_t xiaotai_profile_main(void)
{
    ++calls;
    return result;
}
namespace xiaotai::board {
Board& SelectedBoard()
{
    static SelectedIdentity board({"test", "source"});
    return board;
}
}
int main()
{
    using namespace xiaotai;
    result = ESP_ERR_NO_MEM;
    Application failed;
    assert(failed.Start() == ESP_ERR_NO_MEM);
    assert(failed.phase() == BootPhase::Services);
    assert(failed.error() == ESP_ERR_NO_MEM);
    assert(failed.Start() == ESP_ERR_NO_MEM && calls == 1);
    result = ESP_OK;
    Application success;
    assert(success.Start() == ESP_OK);
    assert(success.phase() == BootPhase::Started);
    assert(success.Start() == ESP_OK && calls == 2);
}
