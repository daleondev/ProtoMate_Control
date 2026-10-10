#pragma once

#include "Tmc2209.hpp"
#include "Tmc2209Bus.hpp"
#include "hal/devices/util/DriverFaultMonitor.hpp"
#include "hal/drivers/itf/IDigitalOutput.hpp"
#include <atomic>

namespace hal::device
{
    class Tmc2209Driver final : public IConfigurableStepperDriver
    {
      public:
        Tmc2209Driver(const Tmc2209Driver&) = delete;
        auto operator=(const Tmc2209Driver&) -> Tmc2209Driver& = delete;
        Tmc2209Driver(Tmc2209Driver&&) = delete;
        auto operator=(Tmc2209Driver&&) -> Tmc2209Driver& = delete;

        Tmc2209Driver(std::shared_ptr<Tmc2209Bus> bus,
                      std::uint8_t address,
                      std::shared_ptr<IDigitalInput> diag,
                      std::shared_ptr<IDigitalOutput> enable,
                      Configuration config,
                      Limits limits);
        ~Tmc2209Driver() override;
        std::string_view name() const noexcept override { return "TMC2209"; }
        Status status() const noexcept override;
        hal::util::Result<> initialize() override;
        hal::util::Result<> verify() override;
        void service() override;
        void setFaultCallback(FaultCallback callback) override { m_fault.setCallback(std::move(callback)); }
        Snapshot snapshot() const override;
        hal::util::Result<> configure(const Configuration& config) override;

      private:
        hal::util::Result<> validate(const Configuration& /*config*/) const;
        void inspectLocked();
        std::shared_ptr<Tmc2209Bus> m_bus;
        std::shared_ptr<IDigitalOutput> m_enable;
        util::DriverFaultMonitor m_fault;
        struct Lock;
        std::unique_ptr<Lock> m_lock;
        Tmc2209 m_device;
        Configuration m_configuration;
        const Limits m_limits; // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members) -- Immutable
                               // calibration/limits for this device lifetime.
        const std::uint16_t m_microsteps; // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members) --
                                          // Immutable calibration/limits for this device lifetime.
        std::optional<Diagnostics> m_diagnostics;
        std::atomic_bool m_warning;
        bool m_initialized{};
    };
}
