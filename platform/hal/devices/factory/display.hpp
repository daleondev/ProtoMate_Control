#pragma once
#include "hal/devices/itf/IDisplay.hpp"
#include "hal/devices/itf/ITouch.hpp"
#include "hal/drivers/itf/GpioTypes.hpp"
#include <memory>
namespace hal::device::display {
    struct PanelConfiguration { gpio::Pin standby{}, reset{}, backlight{}; };
    struct TouchConfiguration { gpio::Pin reset{}, interrupt{}; std::uint8_t address{0x55}; };
    [[nodiscard]] auto create(PanelConfiguration configuration) -> std::shared_ptr<IDisplay>;
    [[nodiscard]] auto create_touch(TouchConfiguration configuration) -> std::shared_ptr<ITouch>;
}
