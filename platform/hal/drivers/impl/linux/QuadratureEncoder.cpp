#include "QuadratureEncoder.hpp"
#include "simulation/Client.hpp"

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
    // Use the same C++ runtime as application threads (see StepHardware.cpp).
    struct QuadratureEncoder::Service
    {
        explicit Service(QuadratureEncoder& encoder)
          : worker{ [&encoder](const std::stop_token& stop) {
              while (!stop.stop_requested()) {
                  encoder.service();
                  std::this_thread::sleep_for(IQuadratureEncoder::SAMPLE_PERIOD);
              }
          } }
        {
        }
        std::jthread worker;
    };

    QuadratureEncoder::QuadratureEncoder(util::TimerLease lease,
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
        simulation::client::Transaction transaction{};
        const std::scoped_lock lock{ m_mutex };
        const auto value{ m_counter.position() };
        if (!value) {
            return std::unexpected(value.error());
        }
        if (!m_running) {
            if (simulation::client::connected())
                simulation::client::command(simulation::Operation::EncoderRun, { 1 });
            m_running = true;
            m_sampler.publish({ .position = value, .timestamp = now(), .running = true });
        }
        return {};
    }
    auto QuadratureEncoder::stop() noexcept -> util::Result<>
    {
        simulation::client::Transaction transaction{};
        const std::scoped_lock lock{ m_mutex };
        if (simulation::client::connected()) {
            simulation::client::command(simulation::Operation::EncoderRun, { 0 });
            service();
        }
        m_running = false;
        m_sampler.publish({ .position = m_counter.position(), .timestamp = now(), .running = false });
        return {};
    }
    auto QuadratureEncoder::isRunning() const noexcept -> bool
    {
        simulation::client::Transaction transaction{};
        const std::scoped_lock lock{ m_mutex };
        return m_running;
    }
    auto QuadratureEncoder::position() const noexcept -> util::Result<Count>
    {
        simulation::client::Transaction transaction{};
        const std::scoped_lock lock{ m_mutex };
        if (auto* memory{ simulation::client::memory() }) {
            if (simulation::client::failed())
                return std::unexpected(std::make_error_code(std::errc::io_error));
            return std::bit_cast<std::int64_t>(simulation::acquire(memory->encoder_count));
        }
        return m_counter.position();
    }
    auto QuadratureEncoder::setPosition(Count count) noexcept -> util::Result<>
    {
        simulation::client::Transaction transaction{};
        const std::scoped_lock lock{ m_mutex };
        if (m_running) {
            return std::unexpected(std::make_error_code(std::errc::device_or_resource_busy));
        }
        if (simulation::client::connected())
            simulation::client::command(simulation::Operation::EncoderReset,
                                        { std::bit_cast<std::uint64_t>(count) });
        m_counter.reset({ .count = count, .raw = m_raw });
        return {};
    }
    auto QuadratureEncoder::setSampleCallback(SampleCallback callback) -> void
    {
        simulation::client::Transaction transaction{};
        const std::scoped_lock lock{ m_mutex };
        m_sampler.setCallback(std::move(callback));
        m_sampler.publish({ .position = m_counter.position(), .timestamp = now(), .running = m_running });
    }
    auto QuadratureEncoder::service() noexcept -> void
    {
        simulation::client::Transaction transaction{};
        const std::scoped_lock lock{ m_mutex };
        if (auto* memory{ simulation::client::memory() }) {
            const auto count{ std::bit_cast<std::int64_t>(simulation::acquire(memory->encoder_count)) };
            m_counter.reset({ .count = count, .raw = static_cast<std::uint16_t>(count) });
        }
        const auto timestamp{ now() };
        if (m_running && m_sampler.due(timestamp)) {
            m_sampler.publish({ .position = m_counter.position(), .timestamp = timestamp, .running = true });
        }
    }
    auto QuadratureEncoder::advanceSimulatedCounts(std::int32_t delta) noexcept -> util::Result<>
    {
        simulation::client::Transaction transaction{};
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
