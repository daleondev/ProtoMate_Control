#include "hal/drivers/factory/intercore.hpp"

namespace hal::intercore
{
    auto create() -> std::shared_ptr<IInterCoreChannel> { return {}; }
}
