#pragma once

#include "hal/devices/itf/IStepperDriver.hpp"
#include <memory>

namespace hal::test
{
    // Explicit substitute for unit tests and the unpowered STEP-signal bench.
    // Never used by the application/board factory. It claims no driver hardware.
    class SignalOnlyDriver final : public device::IStepperDriver
    {
      public:
        std::string_view name() const noexcept override { return "signal test"; }
        Status status() const noexcept override { Status result{}; result.ready = true; return result; }
        util::Result<> initialize() override { return {}; }
        util::Result<> verify() override { return {}; }
        void service() override {}
        void setFaultCallback(FaultCallback) override {}
    };
    inline auto signalOnlyDriver() { return std::make_shared<SignalOnlyDriver>(); }
}
