#pragma once
#include "hal/devices/itf/ITouch.hpp"
#include "hal/drivers/itf/IDigitalInput.hpp"
#include "hal/drivers/itf/IDigitalOutput.hpp"
#include "hal/drivers/itf/II2c.hpp"
#include <memory>
namespace hal::device {
    class St1633i final : public ITouch {
    public:
        St1633i(std::shared_ptr<II2c> bus, std::uint8_t address,
                std::shared_ptr<IDigitalOutput> reset, std::shared_ptr<IDigitalInput> interrupt);
        auto initialize() -> util::Result<> override;
        auto read() -> util::Result<TouchReport> override;
    private:
        std::shared_ptr<II2c> m_bus;
        std::uint8_t m_address;
        std::shared_ptr<IDigitalOutput> m_reset;
        std::shared_ptr<IDigitalInput> m_interrupt;
        bool m_initialized{};
    };
}
