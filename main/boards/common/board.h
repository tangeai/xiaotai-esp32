#pragma once

#include <cstdint>

struct xiaotai_hardware;

namespace xiaotai::board {

struct Identity {
    const char* model;
    const char* revision;
};

// Board owns physical resources. Network identity, sessions and UI belong to
// their services. Return values preserve the platform's esp_err_t error codes.
class Board {
public:
    virtual ~Board() = default;
    virtual Identity identity() const = 0;

protected:
    Board() = default;
    Board(const Board&) = delete;
    Board& operator=(const Board&) = delete;
};

// Codec handles are specific to the I2S/LCD backend. UART Opus and OLED boards
// retain their typed C endpoints rather than manufacturing codec handles.
class CodecBoard : public Board {
public:
    virtual int PrepareAudio() = 0;
    virtual int OpenAudio(xiaotai_hardware& handles) = 0;
    virtual int OpenDisplay(xiaotai_hardware& handles) = 0;
    virtual int SetOutput(std::uint8_t mask, bool high) = 0;
    virtual int SetBacklight(bool awake) = 0;

};

// Exactly one board target defines this function. No heap-allocated factory.
Board& SelectedBoard();
CodecBoard& SelectedCodecBoard();

}  // namespace xiaotai::board
