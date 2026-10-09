#pragma once

#include "IStepperDriver.hpp"

#include <cstdint>
#include <optional>

namespace hal::device
{
    // Stepper driver with an optional configuration/telemetry capability.
    // GPIO-only drivers do not implement it. No raw UART is exposed to callers.
    class IConfigurableStepperDriver : public IStepperDriver
    {
      public:
        enum class Mode { SpreadCycle, StealthChop };
        struct Configuration
        {
            std::uint16_t run_milliamps{ 650 }; // RMS; rounded down by the driver
            std::uint16_t hold_milliamps{ 650 };
            std::uint16_t microsteps{ 16 };
            Mode mode{ Mode::SpreadCycle };
            bool interpolate{ true };
            bool index_step{ false };
            bool operator==(const Configuration&) const = default;
        };
        struct Diagnostics
        {
            std::uint32_t global{}, driver{}, input{};
            std::uint16_t load{};
            bool reset() const { return global & 1U; }
            bool warning() const { return driver & 1U; }
            bool fault() const { return (global & 6U) || (driver & 0x3EU); }
            bool openLoad() const { return driver & 0xC0U; }
            bool standstill() const { return driver & (1U << 31U); }
        };
        struct Limits
        {
            std::uint16_t maximum_run_milliamps;
            bool hold_equals_run;
        };
        struct Snapshot
        {
            Configuration configuration;
            std::optional<Diagnostics> diagnostics;
            Limits limits;
            std::uint8_t address;
            std::uint16_t nominal_run_milliamps{}, nominal_hold_milliamps{};
        };
        ~IConfigurableStepperDriver() override = default;
        IConfigurableStepperDriver* configuration() noexcept final { return this; }
        virtual Snapshot snapshot() const = 0; // Cached; no UART transaction.
        // Disabled only. Validate and stage settings, leaving the driver unready.
        // Explicit initialize() applies/verifies them. Microsteps must continue
        // matching the motor/feedback configuration; INDEX remains a phase marker.
        virtual hal::util::Result<> configure(const Configuration&) = 0;
    };
}
