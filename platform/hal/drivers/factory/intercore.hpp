#pragma once

#include "hal/drivers/itf/IInterCoreChannel.hpp"
#include "hal/util/Core.hpp"
#include <memory>

namespace hal::intercore
{
    // One physical peer link per processor. Host has no hardware endpoint.
    [[nodiscard]] auto create(util::Core owner) -> std::shared_ptr<IInterCoreChannel>;
}
