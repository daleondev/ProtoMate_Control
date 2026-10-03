#include "PwmOutput.hpp"

#include <utility>

namespace hal
{
    PwmOutput::PwmOutput(pwm::Configuration,
                         std::uint8_t,
                         detail::TimerLease lease,
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
        const auto result{ detail::pwmTiming(requested, 240'000'000U) };
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
        if (m_timing.period_ticks == 0U) {
            return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
        }
        m_running = true;
        return {};
    }
    auto PwmOutput::stop() noexcept -> util::Result<>
    {
        const std::scoped_lock lock{ m_mutex };
        m_running = false;
        m_pin->write(gpio::Level::Low);
        return {};
    }
    auto PwmOutput::isRunning() const noexcept -> bool
    {
        const std::scoped_lock lock{ m_mutex };
        return m_running;
    }
}
