#pragma once
#include "hal/devices/itf/IDisplay.hpp"
#include "hal/devices/itf/ITouch.hpp"
#include "hal/drivers/itf/GpioTypes.hpp"
#include <memory>
namespace hal::device::display
{
    inline constexpr std::uint8_t ST1633I_ADDRESS{ 0x55 };
    struct PanelConfiguration
    {
        gpio::Pin standby{}, reset{}, backlight{}, power_ready{};
    };
    struct TouchConfiguration
    {
        gpio::Pin reset{}, interrupt{};
        std::uint8_t address{ ST1633I_ADDRESS };
    };
    [[nodiscard]] auto create(PanelConfiguration configuration) -> std::shared_ptr<IDisplay>;
    [[nodiscard]] auto create_touch(TouchConfiguration configuration) -> std::shared_ptr<ITouch>;
}
