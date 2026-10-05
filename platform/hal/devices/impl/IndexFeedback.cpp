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
        std::int64_t cycles{};
        bool alive{ true }, armed{}, running{}, forward{}, observed{}, fault{};
        gpio::Level level;
        double velocity{};
        std::chrono::nanoseconds period{}, watch_period{};
        std::optional<Clock::time_point> previous;
        std::optional<Clock::time_point> last_edge;
        std::uint64_t interval_cycles{};

        State(Now clock, gpio::Level initial) : now{ clock }, level{ initial } {}

        void publish(Clock::time_point time) noexcept
        {
            if (!callback) return;
            util::Result<std::int64_t> value{ cycles };
            if (fault)
                value = std::unexpected(std::make_error_code(std::errc::state_not_recoverable));
            else if (!observed)
                value = std::unexpected(std::make_error_code(std::errc::no_message_available));
            callback({ value, velocity,
                       std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()) });
        }

        void edge(gpio::Level current) noexcept
        {
            const Guard guard{ mutex };
            if (!alive || current == level) return;
            level = current;
            if (!armed || fault) return;
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

    IndexFeedback::IndexFeedback(std::shared_ptr<IDigitalInput> input, Now now)
      : m_input{ std::move(input) }
    {
        if (!m_input || !now) throw std::invalid_argument("INDEX input/clock unavailable");
        m_state = std::make_shared<State>(now, m_input->read());
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

    void IndexFeedback::motion(bool running, bool forward, std::chrono::nanoseconds cycle_period) noexcept
    {
        const State::Guard guard{ m_state->mutex };
        auto& s{ *m_state };
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

    void IndexFeedback::reference() noexcept
    {
        const State::Guard guard{ m_state->mutex };
        auto& s{ *m_state };
        s.cycles = 0;
        s.velocity = 0;
        s.previous.reset();
        s.fault = false;
        s.publish(s.now());
    }
}
