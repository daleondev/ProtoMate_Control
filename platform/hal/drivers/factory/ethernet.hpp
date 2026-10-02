#pragma once

#include "hal/drivers/itf/IEthernet.hpp"

#include <memory>

namespace hal::ethernet
{
    [[nodiscard]] auto create(IEthernet::Configuration configuration = {}) -> std::shared_ptr<IEthernet>;
}
