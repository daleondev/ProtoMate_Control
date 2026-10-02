#include "hal/drivers/factory/rng.hpp"

#include "hal/drivers/impl/linux/Rng.hpp"

#include <memory>

namespace hal::rng
{
    auto create() -> std::shared_ptr<IRng>
    {
        static auto rng{ std::make_shared<Rng>() };
        return rng;
    }
}
