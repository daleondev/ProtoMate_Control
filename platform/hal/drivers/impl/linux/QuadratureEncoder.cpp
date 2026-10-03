#include "QuadratureEncoder.hpp"

#include <algorithm>
#include <utility>

namespace hal
{
    QuadratureEncoder::QuadratureEncoder(detail::TimerLease lease,
                                         std::shared_ptr<IDigitalInput> a,
                                         std::shared_ptr<IDigitalInput> b)
      : m_lease{ std::move(lease) }
      , m_a{ std::move(a) }
      , m_b{ std::move(b) }
    {
    }
    auto QuadratureEncoder::start() noexcept -> util::Result<>
    {
        const std::scoped_lock lock{ m_mutex };
        const auto value{ m_counter.position() };
        if (!value) {
            return std::unexpected(value.error());
        }
        m_running = true;
        return {};
    }
    auto QuadratureEncoder::stop() noexcept -> util::Result<>
    {
        const std::scoped_lock lock{ m_mutex };
        m_running = false;
        return {};
    }
    auto QuadratureEncoder::isRunning() const noexcept -> bool
    {
        const std::scoped_lock lock{ m_mutex };
        return m_running;
    }
    auto QuadratureEncoder::position() const noexcept -> util::Result<Count>
    {
        const std::scoped_lock lock{ m_mutex };
        return m_counter.position();
    }
    auto QuadratureEncoder::setPosition(Count count) noexcept -> util::Result<>
    {
        const std::scoped_lock lock{ m_mutex };
        if (m_running) {
            return std::unexpected(std::make_error_code(std::errc::device_or_resource_busy));
        }
        m_counter.reset(count, m_raw);
        return {};
    }
    auto QuadratureEncoder::advanceSimulatedCounts(std::int32_t delta) noexcept -> util::Result<>
    {
        const std::scoped_lock lock{ m_mutex };
        while (m_running && delta != 0) {
            const std::int32_t chunk{ std::clamp(delta, -16'384, 16'384) };
            m_raw = static_cast<std::uint16_t>(static_cast<std::int32_t>(m_raw) + chunk);
            m_counter.sample(m_raw);
            delta -= chunk;
            const auto value{ m_counter.position() };
            if (!value) {
                return std::unexpected(value.error());
            }
        }
        return {};
    }
}
