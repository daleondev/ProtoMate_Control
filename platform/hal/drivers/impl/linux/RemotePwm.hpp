#pragma once
#include "hal/drivers/itf/IDigitalOutput.hpp"
#include "hal/drivers/itf/IPwmOutput.hpp"
#include "hal/drivers/util/PwmTiming.hpp"
#include "hal/drivers/util/TimerLease.hpp"
#include "simulation/Client.hpp"
namespace hal
{
    class RemotePwm final : public IPwmOutput
    {
      public:
        RemotePwm(unsigned id,
                  pwm::Configuration configuration,
                  util::TimerLease lease,
                  std::shared_ptr<IDigitalOutput> pin)
          : m_id{ id }
          , m_pin_id{ unsigned(std::to_underlying(configuration.pin.port)) * 16 + configuration.pin.number }
          , m_lease{ std::move(lease) }
          , m_pin{ std::move(pin) }
        {
            simulation::client::Transaction transaction{};
            simulation::client::registerTimer(
              18 + id, this, [](void* p) { static_cast<RemotePwm*>(p)->dispatch(); });
        }
        ~RemotePwm() override
        {
            simulation::client::Transaction transaction{};
            static_cast<void>(stop());
            simulation::client::registerTimer(18 + m_id, nullptr, nullptr);
        }
        util::Result<> configure(Timing requested) noexcept override
        {
            simulation::client::Transaction transaction{};
            if (isRunning())
                return std::unexpected(std::make_error_code(std::errc::device_or_resource_busy));
            auto value{ util::pwm_timing(requested, 240000000) };
            if (!value)
                return std::unexpected(value.error());
            auto response{ simulation::client::command(
              simulation::Operation::PwmConfigure,
              { m_id,
                m_pin_id,
                static_cast<std::uint64_t>(value->actual.period.count()),
                static_cast<std::uint64_t>(value->actual.high_time.count()) }) };
            if (response.status)
                return std::unexpected(std::make_error_code(std::errc::invalid_argument));
            m_timing = value->actual;
            return {};
        }
        Timing timing() const noexcept override { return m_timing; }
        util::Result<> start() noexcept override { return run(0, false); }
        util::Result<> startPulses(std::optional<PulseCount> count) noexcept override
        {
            if (count == 0)
                return std::unexpected(std::make_error_code(std::errc::invalid_argument));
            return run(count.value_or(0), true);
        }
        PulseCount pulseCount() const noexcept override
        {
            return simulation::acquire(simulation::client::memory()->pwm[m_id][0]);
        }
        bool isRunning() const noexcept override
        {
            return simulation::acquire(simulation::client::memory()->pwm[m_id][1]) != 0;
        }
        util::Result<> stop() noexcept override
        {
            simulation::client::Transaction transaction{};
            const auto r{ simulation::client::command(simulation::Operation::PwmStop, { m_id }) };
            return r.status ? util::Result<>{ std::unexpected(std::make_error_code(std::errc::io_error)) }
                            : util::Result<>{};
        }
        util::Result<> setPulseCallback(PulseCallback callback) noexcept override
        {
            simulation::client::Transaction transaction{};
            if (isRunning())
                return std::unexpected(std::make_error_code(std::errc::device_or_resource_busy));
            m_callback = std::move(callback);
            return {};
        }

      private:
        util::Result<> run(PulseCount count, bool counted)
        {
            simulation::client::Transaction transaction{};
            if (isRunning()) {
                if (!m_counted && !counted)
                    return {};
                return std::unexpected(std::make_error_code(std::errc::device_or_resource_busy));
            }
            m_observed = 0;
            m_counted = counted;
            const auto r{ simulation::client::command(simulation::Operation::PwmStart,
                                                      { m_id, count, counted }) };
            return r.status
                     ? util::Result<>{ std::unexpected(std::make_error_code(std::errc::invalid_argument)) }
                     : util::Result<>{};
        }
        void dispatch() noexcept
        {
            const auto count{ pulseCount() };
            if (count != m_observed) {
                m_observed = count;
                if (m_callback)
                    m_callback(count);
            }
        }
        unsigned m_id, m_pin_id;
        util::TimerLease m_lease;
        std::shared_ptr<IDigitalOutput> m_pin;
        Timing m_timing{};
        PulseCount m_observed{};
        bool m_counted{};
        PulseCallback m_callback;
    };
}
