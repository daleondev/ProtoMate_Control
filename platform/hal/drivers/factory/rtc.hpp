#pragma once

#include "hal/drivers/itf/IRtc.hpp"

#include <memory>

namespace hal::rtc
{
    [[nodiscard]] auto create() -> std::shared_ptr<IRtc>;
}
