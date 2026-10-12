#pragma once

#include "hal/drivers/itf/IInterCoreChannel.hpp"
#include <memory>

namespace hal::intercore
{
    // One physical peer link per processor. Host has no hardware endpoint.
    [[nodiscard]] auto create() -> std::shared_ptr<IInterCoreChannel>;
}
