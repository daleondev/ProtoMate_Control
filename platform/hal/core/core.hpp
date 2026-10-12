#pragma once

#include "hal/devices/itf/ILed.hpp"
#include "hal/devices/itf/IButton.hpp"
#include "hal/drivers/itf/IRtc.hpp"
#include "hal/drivers/itf/IRng.hpp"
#include "hal/drivers/itf/ITimer.hpp"
#include "hal/util/Core.hpp"
#include "hal/drivers/itf/IInterCoreChannel.hpp"

#include <memory>

namespace hal::core
{
    using Id = util::Core;

    [[nodiscard]] constexpr auto id() noexcept -> Id { return util::executing_core(); }

    // Only the executing core can be selected. These factories create no
    // peer-core objects and do not start the returned devices.
    [[nodiscard]] auto create_user_button() -> std::shared_ptr<device::IButton>;
    [[nodiscard]] auto create_error_led() -> std::shared_ptr<device::ILed>;
    [[nodiscard]] auto create_rtc() -> std::shared_ptr<IRtc>;
    [[nodiscard]] auto create_rng() -> std::shared_ptr<IRng>;
    [[nodiscard]] auto create_status_led() -> std::shared_ptr<device::ILed>;
    [[nodiscard]] auto create_intercore_channel() -> std::shared_ptr<IInterCoreChannel>;
    [[nodiscard]] auto create_runtime_timer() -> std::shared_ptr<ITimer>;
}
