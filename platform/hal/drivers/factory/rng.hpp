#pragma once

#include "hal/drivers/itf/IRng.hpp"

#include <memory>

namespace hal::rng
{
    [[nodiscard]] auto create() -> std::shared_ptr<IRng>;
}
