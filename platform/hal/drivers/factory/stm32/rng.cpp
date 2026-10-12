#include "hal/drivers/factory/rng.hpp"
#include "hal/drivers/impl/stm32/Rng.hpp"

namespace hal::rng
{
    auto create() -> std::shared_ptr<IRng>
    {
        // Hardware is initialized exactly once by M7 before releasing M4.
        // Handles and thread synchronization remain private to each processor.
        static RNG_HandleTypeDef handle{};
        static auto instance = [] {
            handle.Instance = RNG; handle.State = HAL_RNG_STATE_READY;
            return std::make_shared<Rng>(handle);
        }();
        return instance;
    }
}
