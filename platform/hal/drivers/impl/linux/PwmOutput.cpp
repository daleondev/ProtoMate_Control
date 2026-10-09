#include "PwmOutput.hpp"

#include <utility>

namespace hal
{
    PwmOutput::PwmOutput(pwm::Configuration,
                         std::uint8_t,
                         util::TimerLease lease,
                         std::shared_ptr<IDigitalOutput> pin)
      : m_lease{ std::move(lease) }
      , m_pin{ std::move(pin) }
    {
    }
    PwmOutput::~PwmOutput() { static_cast<void>(stop()); }

    auto PwmOutput::configure(Timing requested) noexcept -> util::Result<>
    {
        const std::scoped_lock lock{ m_mutex };
        if (m_running) {
            return std::unexpected(std::make_error_code(std::errc::device_or_resource_busy));
        }
        const auto result{ util::pwmTiming(requested, 240'000'000U) };
        if (!result) {
            return std::unexpected(result.error());
        }
        m_timing = *result;
        return {};
    }
    auto PwmOutput::timing() const noexcept -> Timing
    {
        const std::scoped_lock lock{ m_mutex };
        return m_timing.actual;
    }
    auto PwmOutput::start() noexcept -> util::Result<>
    {
        const std::scoped_lock lock{ m_mutex };
        if (m_running) {
            if (m_counted) {
                return std::unexpected(std::make_error_code(std::errc::device_or_resource_busy));
            }
            return {};
        }
        if (m_timing.period_ticks == 0U) {
            return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
        }
        m_train.start(0U);
        m_counted = false;
        m_running = true;
        return {};
    }
    auto PwmOutput::startPulses(std::optional<PulseCount> count) noexcept -> util::Result<>
    {
        const std::scoped_lock lock{ m_mutex };
        if (m_running) {
            return std::unexpected(std::make_error_code(std::errc::device_or_resource_busy));
        }
        if (m_timing.period_ticks == 0U) {
            return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
        }
        if (count == 0U || m_timing.high_ticks == 0U || m_timing.high_ticks == m_timing.period_ticks) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        m_train.start(count);
        m_counted = true;
        m_running = true;
        return {};
    }
    auto PwmOutput::pulseCount() const noexcept -> PulseCount
    {
        const std::scoped_lock lock{ m_mutex };
        return m_train.count();
    }
    auto PwmOutput::setPulseCallback(PulseCallback callback) noexcept -> util::Result<>
    {
        const std::scoped_lock lock{ m_mutex };
        if (m_running) {
            return std::unexpected(std::make_error_code(std::errc::device_or_resource_busy));
        }
        m_callback = std::move(callback);
        return {};
    }
    auto PwmOutput::stop() noexcept -> util::Result<>
    {
        const std::scoped_lock lock{ m_mutex };
        m_running = false;
        m_train.stop();
        m_pin->write(gpio::Level::Low);
        return {};
    }
    auto PwmOutput::isRunning() const noexcept -> bool
    {
        const std::scoped_lock lock{ m_mutex };
        return m_running;
    }
    auto PwmOutput::beginSimulatedPulse() noexcept -> void
    {
        const std::scoped_lock lock{ m_mutex };
        if (m_running && m_counted && m_train.rise() && m_callback) {
            m_callback(m_train.count());
        }
    }
    auto PwmOutput::finishSimulatedPulse() noexcept -> void
    {
        const std::scoped_lock lock{ m_mutex };
        if (m_running && m_counted) {
            // Like an update IRQ, drain a rising edge whose IRQ was not serviced.
            beginSimulatedPulse();
            m_running = m_train.finishPulse();
        }
    }
    auto PwmOutput::advanceSimulatedPulses(PulseCount count) noexcept -> void
    {
        const std::scoped_lock lock{ m_mutex };
        while (count != 0U && m_running && m_counted) {
            finishSimulatedPulse();
            --count;
        }
    }
}
