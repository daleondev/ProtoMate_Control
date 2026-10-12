#include "hal/drivers/factory/rng.hpp"

namespace hal::rng
{
    auto create() -> std::shared_ptr<IRng>
    {
        // No core owns this shared peripheral yet.
        return {};
    }
}
