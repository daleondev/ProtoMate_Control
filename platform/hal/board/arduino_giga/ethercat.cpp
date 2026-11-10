#include "hal/board/board.hpp"
#include "hal/drivers/factory/gpio.hpp"
#include "hal/drivers/factory/spi.hpp"
namespace hal::board
{
    namespace
    {
        constexpr gpio::Pin CHIP_SELECT{ .port = gpio::Port::K, .number = 1 };
        constexpr gpio::Pin RESET{ .port = gpio::Port::H, .number = 9 };
        constexpr gpio::Pin INTERRUPT{ .port = gpio::Port::H, .number = 10 };
        constexpr gpio::Pin SYNC0{ .port = gpio::Port::H, .number = 11 };
    }
    auto create_ethercat_spi() -> std::shared_ptr<ISpi> { return spi::create({ spi::Peripheral::Spi5 }); }
    auto create_ethercat_chip_select() -> std::shared_ptr<IDigitalOutput>
    {
        return gpio::create_output({ .pin = CHIP_SELECT, .initial_level = gpio::Level::High });
    }
    auto create_ethercat_reset() -> std::shared_ptr<IDigitalOutput>
    {
        return gpio::create_output(
          { .pin = RESET, .initial_level = gpio::Level::High, .type = gpio::OutputType::OpenDrain });
    }
    auto create_ethercat_interrupt() -> std::shared_ptr<IDigitalInput>
    {
        return gpio::create_input({ .pin = INTERRUPT, .pull = gpio::Pull::Up, .edge = gpio::Edge::Falling });
    }
    auto create_ethercat_sync0() -> std::shared_ptr<IDigitalInput>
    {
        return gpio::create_input({ .pin = SYNC0, .edge = gpio::Edge::Rising });
    }
}
