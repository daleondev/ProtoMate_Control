#pragma once

#include "hal/devices/impl/linux/Tmc2209Uart.hpp"
#include <memory>

namespace hal::board
{
    // Linux-only access to the device model attached by createStepperDriverBus.
    [[nodiscard]] auto simulatedStepperBus() -> std::shared_ptr<device::Tmc2209Uart>;
}
