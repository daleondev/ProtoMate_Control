#pragma once

#include "hal/util/Result.hpp"

#include <functional>
#include <string_view>

namespace hal::device
{
    class IConfigurableStepperDriver;

    class IStepperDriver
    {
      public:
        enum class Fault { None, Input, Communication, Configuration, Electrical, Reset };
        struct Status
        {
            bool ready{}, fault_active{}, fault_latched{}, warning{};
            Fault fault{ Fault::None };
            std::error_code error;
        };
        using FaultCallback = std::move_only_function<void() noexcept>;
        virtual ~IStepperDriver() = default;
        virtual std::string_view name() const noexcept = 0;
        // No UART or blocking work. Suitable for motion readiness checks and the
        // short interrupt-masked shared-enable decision. GPIO level is live.
        virtual Status status() const noexcept = 0;
        // Disabled only; explicit initialization/recovery never enables a motor.
        virtual hal::util::Result<> initialize() = 0;
        virtual hal::util::Result<> verify() = 0;
        virtual void service() = 0; // Thread context: poll extended diagnostics.
        virtual IConfigurableStepperDriver* configuration() noexcept { return nullptr; }
        // One observer; registration delivers an already latched fault. May run
        // in an ISR: no blocking, logging, allocation or re-entry into this driver.
        // Clearing synchronizes with in-flight callbacks. A latch clears ONLY
        // through explicit initialization while disabled, never on pin release.
        virtual void setFaultCallback(FaultCallback) = 0;
        void clearFaultCallback() { setFaultCallback({}); }
    };
}
