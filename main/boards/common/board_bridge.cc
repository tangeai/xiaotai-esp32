#include "board.h"
#include "xiaotai_hardware.h"

using xiaotai::board::SelectedCodecBoard;

extern "C" esp_err_t xiaotai_board_audio_prepare(void)
{
    return SelectedCodecBoard().PrepareAudio();
}

extern "C" esp_err_t xiaotai_board_audio_open(xiaotai_hardware_t* handles)
{
    return handles ? SelectedCodecBoard().OpenAudio(*handles) : ESP_ERR_INVALID_ARG;
}

extern "C" esp_err_t xiaotai_board_display_open(xiaotai_hardware_t* handles)
{
    return handles ? SelectedCodecBoard().OpenDisplay(*handles) : ESP_ERR_INVALID_ARG;
}

extern "C" esp_err_t xiaotai_board_output_set(uint8_t mask, bool high)
{
    return SelectedCodecBoard().SetOutput(mask, high);
}

extern "C" esp_err_t xiaotai_board_backlight_set(bool awake)
{
    return SelectedCodecBoard().SetBacklight(awake);
}
