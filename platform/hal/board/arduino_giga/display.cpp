#include "hal/board/board.hpp"
#include "hal/devices/factory/display.hpp"
namespace hal::board {
    auto create_display() -> std::shared_ptr<device::IDisplay>
    { return device::display::create({.standby={gpio::Port::I,0}, .reset={gpio::Port::I,2}, .backlight={gpio::Port::C,6}}); }
    auto create_touch() -> std::shared_ptr<device::ITouch>
    { return device::display::create_touch({.reset={gpio::Port::I,3}, .interrupt={gpio::Port::B,12}, .address=0x55}); }
}
