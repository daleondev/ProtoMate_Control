#pragma once

#include "hal/devices/itf/IStepperDriver.hpp"
#include "hal/drivers/itf/IDigitalInput.hpp"

#include <memory>

namespace hal::device::detail
{
    // Shared ALM/DIAG latching and callback lifetime handling. No UART here.
    class DriverFaultMonitor final
    {
      public:
        explicit DriverFaultMonitor(std::shared_ptr<IDigitalInput> input);
        ~DriverFaultMonitor();
        IStepperDriver::Status status() const noexcept;
        void setCallback(IStepperDriver::FaultCallback callback);
        void checkInput() noexcept;
        void fail(IStepperDriver::Fault fault, std::error_code error) noexcept;
        void beginRecovery() noexcept;
        util::Result<> completeRecovery() noexcept;
        void unready() noexcept;
      private:
        struct State;
        std::shared_ptr<IDigitalInput> m_input;
        std::shared_ptr<State> m_state;
    };
}
