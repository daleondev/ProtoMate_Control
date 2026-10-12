#include "hal/drivers/factory/rng.hpp"

#if !defined(HAL_BOARD_NUCLEO_H755ZIQ)
#include "hal/drivers/impl/stm32/Rng.hpp"
#endif
#include "hal/hal.hpp"

#include <memory>

namespace hal::rng
{
    auto create() -> std::shared_ptr<IRng>
    {
#if defined(HAL_BOARD_NUCLEO_H755ZIQ)
        // No core owns the RNG yet. Never silently emulate random hardware.
        return {};
#else
        static auto rng{ std::make_shared<Rng>(hrng) };
        return rng;
#endif
    }
}
