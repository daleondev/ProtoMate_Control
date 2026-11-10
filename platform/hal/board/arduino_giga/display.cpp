#include "hal/devices/factory/display.hpp"
#include "hal/board/board.hpp"
#include "hal/drivers/factory/graphics.hpp"
namespace hal::board
{
    namespace
    {
        constexpr device::display::PanelConfiguration PANEL{
            .standby = { .port = gpio::Port::I, .number = 0 },
            .reset = { .port = gpio::Port::I, .number = 2 },
            .backlight = { .port = gpio::Port::C, .number = 6 },
            .power_ready = { .port = gpio::Port::I, .number = 1 }
        };
        constexpr device::display::TouchConfiguration TOUCH{ .reset = { .port = gpio::Port::I, .number = 3 },
                                                             .interrupt = { .port = gpio::Port::B,
                                                                            .number = 12 },
                                                             .address = device::display::ST1633I_ADDRESS };
    }
    auto create_graphics() -> std::shared_ptr<IGraphics>
    {
        return graphics::create(graphics::Peripheral::Dma2d);
    }
    auto create_display() -> std::shared_ptr<device::IDisplay> { return device::display::create(PANEL); }
    auto create_touch() -> std::shared_ptr<device::ITouch> { return device::display::create_touch(TOUCH); }
}
