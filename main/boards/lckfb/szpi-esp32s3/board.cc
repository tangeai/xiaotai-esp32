#include "board.h"
#include "xiaotai_hardware.h"

extern "C" {
esp_err_t lck_audio_prepare(void);
esp_err_t lck_audio_open(void);
esp_err_t lck_display_open(void);
esp_err_t lck_output_set(uint8_t mask, bool high);
esp_err_t lck_backlight_set(bool awake);
void lck_hardware_get(xiaotai_hardware_t* handles);
}

namespace xiaotai::board {
namespace {

class SzpiBoard final : public CodecBoard {
public:
    Identity identity() const override { return {"lckfb-szpi-esp32s3", "1.0.1"}; }

    int PrepareAudio() override
    {
        return lck_audio_prepare();
    }

    int OpenAudio(xiaotai_hardware& handles) override
    {
        const auto error = lck_audio_open();
        if (error == ESP_OK) {
            lck_hardware_get(&handles);
        }
        return error;
    }

    int OpenDisplay(xiaotai_hardware& handles) override
    {
        const auto error = lck_display_open();
        if (error == ESP_OK) {
            lck_hardware_get(&handles);
        }
        return error;
    }

    int SetOutput(std::uint8_t mask, bool high) override { return lck_output_set(mask, high); }
    int SetBacklight(bool awake) override { return lck_backlight_set(awake); }

};

}  // namespace

CodecBoard& SelectedCodecBoard()
{
    static SzpiBoard board;
    return board;
}

Board& SelectedBoard() { return SelectedCodecBoard(); }

}  // namespace xiaotai::board
