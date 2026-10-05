#pragma once

#include "hal/drivers/itf/IUart.hpp"
#include <memory>

namespace hal::device
{
    // Adafruit #6121: external 0.05-ohm sense resistors. All operations are
    // thread-context only and externally serialized across the shared UART.
    class Tmc2209 final
    {
      public:
        enum class Mode
        {
            SpreadCycle,
            StealthChop
        };
        struct Configuration
        {
            std::uint16_t run_milliamps{ 650 }; // RMS; nominal, rounded DOWN
            std::uint16_t hold_milliamps{ 650 };
            std::uint16_t microsteps{ 16 };
            Mode mode{ Mode::SpreadCycle };
            bool interpolate{ true };
            bool operator==(const Configuration&) const = default;
        };
        struct Status
        {
            std::uint32_t global{}, driver{}, input{};
            std::uint16_t load{};
            bool reset() const { return global & 1U; }
            bool warning() const { return driver & 1U; }
            bool fault() const { return (global & 6U) || (driver & 0x3EU); }
            bool openLoad() const { return driver & 0xC0U; }
            bool standstill() const { return driver & (1U << 31U); }
        };

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
