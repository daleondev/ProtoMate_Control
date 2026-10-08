#pragma once

#include "Tmc2209.hpp"
#include "Tmc2209Bus.hpp"
#include "hal/devices/detail/DriverFaultMonitor.hpp"
#include "hal/drivers/itf/IDigitalOutput.hpp"
#include <atomic>

namespace hal::device
{
    class Tmc2209Driver final : public IConfigurableStepperDriver
    {
      public:
        Tmc2209Driver(std::shared_ptr<Tmc2209Bus> bus, std::uint8_t address,
                      std::shared_ptr<IDigitalInput> diag, std::shared_ptr<IDigitalOutput> enable,
                      Configuration config, Limits limits);
        ~Tmc2209Driver() override;
        std::string_view name() const noexcept override { return "TMC2209"; }
        Status status() const noexcept override;
        util::Result<> initialize() override;
        util::Result<> verify() override;
        void service() override;
        void setFaultCallback(FaultCallback callback) override { m_fault.setCallback(std::move(callback)); }
        Snapshot snapshot() const override;
        util::Result<> configure(const Configuration&) override;
      private:
        util::Result<> validate(const Configuration&) const;
        void inspectLocked();
        std::shared_ptr<Tmc2209Bus> m_bus;
        std::shared_ptr<IDigitalOutput> m_enable;
        detail::DriverFaultMonitor m_fault;
        struct Lock;
        std::unique_ptr<Lock> m_lock;
        Tmc2209 m_device;
        Configuration m_configuration;
        const Limits m_limits;
        const std::uint16_t m_microsteps;
        std::optional<Diagnostics> m_diagnostics;
        std::atomic_bool m_warning{};
        bool m_initialized{};
    };
}
