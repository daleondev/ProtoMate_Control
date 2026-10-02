#include "hal/drivers/factory/rng.hpp"

#include "hal/drivers/impl/stm32/Rng.hpp"
#include "hal/hal.hpp"

#include <memory>

namespace hal::rng
{
    auto create() -> std::shared_ptr<IRng>
    {
        static auto rng{ std::make_shared<Rng>(hrng) };
        return rng;
    }
}
