#pragma once

#include "hal/devices/itf/ILed.hpp"
#include "hal/drivers/itf/ITimer.hpp"
#include "hal/util/Core.hpp"

#include <memory>

namespace hal::core
{
    using Id = util::Core;

    [[nodiscard]] constexpr auto id() noexcept -> Id { return util::executing_core(); }

    // Only the executing core can be selected. These factories create no
    // peer-core objects and do not start the returned devices.
    [[nodiscard]] auto create_status_led() -> std::shared_ptr<device::ILed>;
    [[nodiscard]] auto create_runtime_timer() -> std::shared_ptr<ITimer>;
}
