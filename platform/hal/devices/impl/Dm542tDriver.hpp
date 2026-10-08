#pragma once

#include "hal/devices/detail/DriverFaultMonitor.hpp"
#include "hal/drivers/itf/IDigitalOutput.hpp"

namespace hal::device
{
    class Dm542tDriver final : public IStepperDriver
    {
      public:
        Dm542tDriver(std::shared_ptr<IDigitalInput> alarm, std::shared_ptr<IDigitalOutput> enable);
        std::string_view name() const noexcept override { return "DM542T"; }
        Status status() const noexcept override { return m_fault.status(); }
        util::Result<> initialize() override;
        util::Result<> verify() override;
        void service() override { m_fault.checkInput(); }
        void setFaultCallback(FaultCallback callback) override { m_fault.setCallback(std::move(callback)); }
      private:
        // Shared enable is observed, never written by an individual driver.
        std::shared_ptr<IDigitalOutput> m_enable;
        detail::DriverFaultMonitor m_fault;
    };
}
