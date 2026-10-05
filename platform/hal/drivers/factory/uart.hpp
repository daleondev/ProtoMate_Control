#pragma once
#include "hal/drivers/itf/IUart.hpp"
#include <memory>

namespace hal::uart
{
    // Exclusive USART2 bus, PD5 TX / PD6 RX, 115200 8N1, external echo.
    std::shared_ptr<IUart> createStepperBus();
}
