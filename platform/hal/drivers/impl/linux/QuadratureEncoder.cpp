#include "QuadratureEncoder.hpp"

#include <algorithm>
#include <thread>
#include <utility>

namespace hal
{
    namespace
    {
        auto now() noexcept -> std::chrono::nanoseconds
        {
            return std::chrono::steady_clock::now().time_since_epoch();
        }
    }
    // Use the same C++ runtime as application threads (see StepService.cpp).
    struct QuadratureEncoder::Service
    {
        explicit Service(QuadratureEncoder& encoder)
          : worker{ [&encoder](std::stop_token stop) {
              while (!stop.stop_requested()) {
                  encoder.service();
                  std::this_thread::sleep_for(IQuadratureEncoder::sample_period);
              }
          } }
        {
        }
        std::jthread worker;
    };

    QuadratureEncoder::QuadratureEncoder(detail::TimerLease lease,
                                         std::shared_ptr<IDigitalInput> a,
                                         std::shared_ptr<IDigitalInput> b)
      : m_lease{ std::move(lease) }
      , m_a{ std::move(a) }
      , m_b{ std::move(b) }
      , m_service{ std::make_unique<Service>(*this) }
    {
    }

    QuadratureEncoder::~QuadratureEncoder()
    {
        m_service.reset(); // Join before callbacks/counter/locks are destroyed.
    }

    auto QuadratureEncoder::start() noexcept -> util::Result<>
    {
        const std::scoped_lock lock{ m_mutex };
        const auto value{ m_counter.position() };
        if (!value) {
            return std::unexpected(value.error());
        }
        if (!m_running) {
            m_running = true;
            m_sampler.publish({ value, now(), true });
        }
        return {};
    }
    auto QuadratureEncoder::stop() noexcept -> util::Result<>
    {
        const std::scoped_lock lock{ m_mutex };
        m_running = false;
        m_sampler.publish({ m_counter.position(), now(), false });
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
    auto QuadratureEncoder::setSampleCallback(SampleCallback callback) -> void
    {
        const std::scoped_lock lock{ m_mutex };
        m_sampler.setCallback(std::move(callback));
        m_sampler.publish({ m_counter.position(), now(), m_running });
    }
    auto QuadratureEncoder::service() noexcept -> void
    {
        const std::scoped_lock lock{ m_mutex };
        const auto timestamp{ now() };
        if (m_running && m_sampler.due(timestamp))
            m_sampler.publish({ m_counter.position(), timestamp, true });
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
