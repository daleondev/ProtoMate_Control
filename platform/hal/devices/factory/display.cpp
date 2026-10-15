#include "hal/devices/factory/display.hpp"
#include "hal/devices/impl/SmartwinDisplay.hpp"
#include "hal/devices/impl/St1633i.hpp"
#include "hal/drivers/factory/gpio.hpp"
#include "hal/drivers/factory/rgb.hpp"
#include "hal/drivers/factory/i2c.hpp"
#if defined(HAL_PLATFORM_LINUX)
#include "hal/linux/DisplayHost.hpp"
#include "hal/devices/impl/PointerTouch.hpp"
#endif
namespace hal::device::display {
    auto create(PanelConfiguration configuration) -> std::shared_ptr<IDisplay> {
        auto output = rgb::create({.backlight=configuration.backlight});
        if (!output) { return {}; }
        auto standby = gpio::create_output({.pin=configuration.standby});
        auto reset = gpio::create_output({.pin=configuration.reset});
        if (!standby || !reset) { return {}; }
        return std::make_shared<SmartwinDisplay>(std::move(output), std::move(standby), std::move(reset));
    }
    auto create_touch(TouchConfiguration configuration) -> std::shared_ptr<ITouch> {
#if defined(HAL_PLATFORM_LINUX)
        static_cast<void>(configuration);
        if (!hal::linux::display::available()) { return {}; }
        return std::make_shared<PointerTouch>([] {
            const auto point = hal::linux::display::pointer();
            TouchReport report{}; report.cancelled=point.cancelled;
            report.droppedReports=hal::linux::display::inputDrops();
            report.points[0] = {.active=point.down, .x=point.x, .y=point.y};
            return report;
        });
#else
        auto bus = i2c::create(i2c::Peripheral::I2c4);
        if (!bus || configuration.address > 0x7F) { return {}; }
        auto reset = gpio::create_output({.pin=configuration.reset});
        auto interrupt = gpio::create_input({.pin=configuration.interrupt, .pull=gpio::Pull::Up, .edge=gpio::Edge::Falling});
        if (!reset || !interrupt) { return {}; }
        return std::make_shared<St1633i>(std::move(bus), configuration.address, std::move(reset), std::move(interrupt));
#endif
    }
}
