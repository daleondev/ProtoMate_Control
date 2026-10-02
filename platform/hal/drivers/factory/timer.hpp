#pragma once

#include "hal/drivers/itf/ITimer.hpp"

#include <cstddef>
#include <memory>

namespace hal::timer
{
    [[nodiscard]] auto create(std::size_t index) -> std::shared_ptr<ITimer>;
}
