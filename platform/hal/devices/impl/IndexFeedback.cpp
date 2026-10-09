#include "IndexFeedback.hpp"

#ifdef HAL_PLATFORM_STM32
#include "hal/stm32/InterruptGuard.hpp"
#else
#include <mutex>
#endif

#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace hal::device
{
    using namespace pnm::units::literals;

    struct IndexFeedback::State
    {
#ifdef HAL_PLATFORM_STM32
        struct Mutex {};
        struct Guard
        {
            explicit Guard(Mutex&) noexcept {}
            stm32::InterruptGuard interrupt_guard;
        };
#else
        using Mutex = std::mutex;
        using Guard = std::lock_guard<Mutex>;
#endif
        Mutex mutex;
        Now now;
        Callback callback;
        const pnm::units::Angle cycle_angle;
        const std::size_t microsteps;
        pnm::units::Angle offset{};
        std::int64_t cycles{};
        bool alive{ true }, started{}, armed{}, running{}, forward{}, observed{}, fault{};
        gpio::Level level;
        double velocity{};
        std::chrono::nanoseconds period{}, watch_period{};
        std::optional<Clock::time_point> previous;
        std::optional<Clock::time_point> last_edge;
        std::uint64_t interval_cycles{};

        State(Now clock, gpio::Level initial, pnm::units::Angle angle, std::size_t steps)
          : now{ clock }, cycle_angle{ angle * 4.0 }, microsteps{ steps }, level{ initial } {}

        void publish(Clock::time_point time) noexcept
        {
            const auto position{ offset + cycle_angle * static_cast<double>(cycles) };
            auto angular_velocity{ cycle_angle * velocity / 1_s };
            if (!position.isFinite() || !angular_velocity.isFinite()) {
                fault = true;
                velocity = 0;
            }
            if (fault || !observed) angular_velocity = 0_rpm;
            if (!callback) return;
            hal::util::Result<pnm::units::Angle> value{ position };
            if (fault)
                value = std::unexpected(std::make_error_code(std::errc::state_not_recoverable));
            else if (!observed)
                value = std::unexpected(std::make_error_code(std::errc::no_message_available));
            callback({ value, angular_velocity,
                       std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()) });
        }

        void edge(gpio::Level current) noexcept
        {
            const Guard guard{ mutex };
            if (!alive || current == level) return;
            level = current;
            if (!started || !armed || fault) return;
            // Use the SAME boundary of the INDEX window in both directions.
            // A forward rising crossing is undone by a reverse falling crossing;
            // counting rising edges in both directions would drift on reversals.
            if ((current == gpio::Level::High) != forward) return;
            const auto time{ now() };
            if (observed) {
                if ((forward && cycles == std::numeric_limits<std::int64_t>::max()) ||
                    (!forward && cycles == std::numeric_limits<std::int64_t>::min())) {
                    fault = true;
                    velocity = 0;
                    publish(time);
                    return;
                }
                cycles += forward ? 1 : -1;
            }
            // The first boundary establishes electrical phase, not a whole
            // revolution of motion from an unknown initial phase.
            observed = true;
            // The embedded steady clock has 1 ms resolution. Accumulate all
            // crossings in at least 10 ms instead of losing cycles when two
            // interrupts receive the same timestamp at high STEP rates.
            if (running) {
                if (!previous) {
                    previous = time;
                    interval_cycles = 0;
                }
                else {
                    ++interval_cycles;
                    if (time - *previous >= std::chrono::milliseconds{ 10 }) {
                        velocity = (forward ? 1.0 : -1.0) * static_cast<double>(interval_cycles) /
                                   std::chrono::duration<double>(time - *previous).count();
                        previous = time;
                        interval_cycles = 0;
                    }
                }
            }
            else {
                velocity = 0;
                previous.reset();
            }
            last_edge = time;
            watch_period = period;
            publish(time);
        }
    };

    IndexFeedback::IndexFeedback(std::shared_ptr<IDigitalInput> input, pnm::units::Angle angle,
                                 std::size_t microsteps, Now now)
      : m_input{ std::move(input) }
    {
        if (!m_input || !now || !angle.isFinite() || angle <= 0_deg || !(angle * 4.0).isFinite() || !microsteps)
            throw std::invalid_argument("invalid INDEX input, clock or motor configuration");
        m_state = std::make_shared<State>(now, m_input->read(), angle, microsteps);
        // Shared state survives a host GPIO callback already in flight during
        // destruction. It never captures the provider or its owner by raw pointer.
        m_input->setEdgeCallback([state = m_state](gpio::Level level) noexcept { state->edge(level); });
    }

    IndexFeedback::~IndexFeedback()
    {
        m_input->clearEdgeCallback();
        const State::Guard guard{ m_state->mutex };
        m_state->alive = false;
        m_state->callback = {};
    }

    void IndexFeedback::setCallback(Callback callback)
    {
        const State::Guard guard{ m_state->mutex };
        m_state->callback = std::move(callback);
        m_state->publish(m_state->now());
    }

    pnm::units::Angle IndexFeedback::resolution() const noexcept { return m_state->cycle_angle; }

    hal::util::Result<> IndexFeedback::start() noexcept
    {
        const State::Guard guard{ m_state->mutex };
        m_state->started = true;
        m_state->publish(m_state->now());
        return {};
    }

    hal::util::Result<> IndexFeedback::stop() noexcept
    {
        const State::Guard guard{ m_state->mutex };
        auto& s{ *m_state };
        s.started = s.armed = s.running = false;
        s.velocity = 0;
        s.previous.reset();
        s.publish(s.now());
        return {};
    }

    void IndexFeedback::motion(bool running, bool forward, std::chrono::nanoseconds step_period) noexcept
    {
        const State::Guard guard{ m_state->mutex };
        auto& s{ *m_state };
        if (!s.started) return;
        auto cycle_period{ std::chrono::nanoseconds::max() };
        const auto maximum{ cycle_period.count() };
        if (step_period.count() >= 0 && s.microsteps <= static_cast<std::uint64_t>(maximum / 4)) {
            const auto multiplier{ static_cast<std::int64_t>(s.microsteps) * 4 };
            if (step_period.count() <= maximum / multiplier)
                cycle_period = step_period * multiplier;
        }
        const auto time{ s.now() };
        if (running && (!s.running || forward != s.forward)) {
            s.previous.reset();
            s.last_edge = time;
            s.velocity = 0;
            s.watch_period = cycle_period;
        }
        s.period = cycle_period;
        s.watch_period = std::max(s.watch_period, cycle_period);
        if (running) {
            s.armed = true;
            s.forward = forward;
        }
        s.running = running;
        if (!running) {
            s.previous.reset();
            s.velocity = 0;
        }
        // Progress callbacks also check freshness; a disconnected INDEX must
        // not leave an old nonzero velocity looking like live feedback.
        // Allow three electrical periods and interpolation/IRQ settling time.
        // Use the slowest period since the last boundary so accelerating cannot
        // make an earlier, legitimately slow interval look like lost feedback.
        if (running && s.observed && s.last_edge && s.watch_period.count() > 0 &&
            s.watch_period < std::chrono::nanoseconds::max() / 4 &&
            time - *s.last_edge > std::max(s.watch_period * 3, std::chrono::nanoseconds{ 20'000'000 })) {
            s.fault = true; // Lost cycles cannot be reconstructed from INDEX.
            s.velocity = 0;
        }
        s.publish(time);
    }

    void IndexFeedback::invalidate() noexcept
    {
        const State::Guard guard{ m_state->mutex };
        auto& s{ *m_state };
        s.armed = s.running = s.observed = s.fault = false;
        s.cycles = 0;
        s.velocity = 0;
        s.previous.reset();
        s.last_edge.reset();
        s.publish(s.now());
    }

    hal::util::Result<> IndexFeedback::reference(pnm::units::Angle position)
    {
        if (!position.isFinite())
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        const State::Guard guard{ m_state->mutex };
        auto& s{ *m_state };
        if (s.running)
            return std::unexpected(std::make_error_code(std::errc::device_or_resource_busy));
        s.offset = position;
        s.cycles = 0;
        s.velocity = 0;
        s.previous.reset();
        s.fault = false;
        s.publish(s.now());
        return {};
    }
}
