#pragma once
#include "board.h"

namespace xiaotai::board {
// Physical C drivers retain their typed resource owners. The selected build
// exposes identity here without pretending UART/OLED devices are I2S/LCD codecs.
class SelectedIdentity final : public Board {
public:
    explicit SelectedIdentity(Identity value) : value_(value) {}
    Identity identity() const override { return value_; }
private:
    Identity value_;
};
}
