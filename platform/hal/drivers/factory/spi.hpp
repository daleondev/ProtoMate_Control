#pragma once
#include "hal/drivers/itf/ISpi.hpp"
#include <memory>

namespace hal::spi
{
    // SPI5: PF7 SCK, PF8 MISO, PF9 MOSI; mode 0, 8 bits, MSB first,
    // APB2 / 128 (937.5 kHz). No automatic chip-select or startup transfers.
    // One owner; the Linux factory reports unavailable (protocol tests inject a fake).
    [[nodiscard]] std::shared_ptr<ISpi> createEthercatBus();
}
