#pragma once

#include "hal/drivers/itf/IUart.hpp"
#include "hal/devices/itf/IConfigurableStepperDriver.hpp"
#include <memory>

namespace hal::device
{
    // Adafruit #6121: external 0.05-ohm sense resistors. All operations are
    // thread-context only and externally serialized across the shared UART.
    class Tmc2209 final
    {
      public:
        using Mode = IConfigurableStepperDriver::Mode;
        using Configuration = IConfigurableStepperDriver::Configuration;
        using Status = IConfigurableStepperDriver::Diagnostics;

        Tmc2209(std::shared_ptr<IUart> bus, std::uint8_t address);
        // Set every node's reply delay before any reads on a multi-node bus.
        static util::Result<> prepareBus(IUart& bus, std::span<const std::uint8_t> addresses);
        // Caller must keep EN high and STEP stopped for initialization.
        util::Result<> initialize(const Configuration& configuration);
        util::Result<> verify();
        util::Result<Status> status();
        std::uint8_t address() const { return m_address; }
        bool initialized() const { return m_initialized; }
        const Configuration& configuration() const { return m_configuration; }
        static util::Result<std::uint8_t> currentScale(std::uint16_t milliamps);
        static std::uint16_t currentMilliamps(std::uint8_t scale);
        static std::uint8_t crc(std::span<const std::uint8_t> bytes);

      private:
        util::Result<std::uint32_t> read(std::uint8_t reg);
        util::Result<> write(std::uint8_t reg, std::uint32_t value);
        std::shared_ptr<IUart> m_bus;
        std::uint8_t m_address;
        Configuration m_configuration;
        std::uint32_t m_gconf{}, m_chopconf{};
        bool m_initialized{};
    };
}
