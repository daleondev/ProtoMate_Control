#include "hal/board/board.hpp"
#include "hal/drivers/factory/spi.hpp"
#include "hal/drivers/factory/gpio.hpp"
namespace hal::board {
    auto create_ethercat_spi() -> std::shared_ptr<ISpi> { return spi::create({spi::Peripheral::Spi5}); }
    auto create_ethercat_chip_select() -> std::shared_ptr<IDigitalOutput>
    { return gpio::create_output({.pin={gpio::Port::K,1}, .initial_level=gpio::Level::High}); }
    auto create_ethercat_reset() -> std::shared_ptr<IDigitalOutput>
    { return gpio::create_output({.pin={gpio::Port::H,9}, .initial_level=gpio::Level::High, .type=gpio::OutputType::OpenDrain}); }
    auto create_ethercat_interrupt() -> std::shared_ptr<IDigitalInput>
    { return gpio::create_input({.pin={gpio::Port::H,10}, .pull=gpio::Pull::Up, .edge=gpio::Edge::Falling}); }
    auto create_ethercat_sync0() -> std::shared_ptr<IDigitalInput>
    { return gpio::create_input({.pin={gpio::Port::H,11}, .edge=gpio::Edge::Rising}); }
}
