#include "hal/drivers/detail/StepGenerator.hpp"

#if defined(HAL_PLATFORM_STM32)
#include "hal/stm32/InterruptGuard.hpp"
#define STEP_LOCK const stm32::InterruptGuard lock
#else
#define STEP_LOCK                                                                                            \
    const std::scoped_lock lock { m_mutex }
#endif

#include <algorithm>
#include <limits>
#include <utility>

namespace hal::detail
{
    namespace
    {
        auto fail(std::errc error) { return std::unexpected(std::make_error_code(error)); }
        class StepOutput final : public IStepOutput
        {
          public:
            StepOutput(std::shared_ptr<StepGenerator> generator, std::size_t axis)
              : m_generator{ std::move(generator) }
              , m_axis{ axis }
            {
            }
            auto prepare(step::Timing timing, std::optional<step::PulseCount> count)
              -> util::Result<> override
            {
                return m_generator->prepare(m_axis, std::span{ &timing, 1U }, count);
            }
            auto prepareSequence(std::span<const step::Timing> sequence) -> util::Result<> override
            {
                return m_generator->prepare(m_axis, sequence, sequence.size());
            }
            auto clear() noexcept -> util::Result<> override { return m_generator->clear(m_axis); }
            auto pulseCount() noexcept -> util::Result<step::PulseCount> override
            {
                const auto value{ m_generator->status() };
                if (!value.counts_exact) {
                    return fail(std::errc::io_error);
                }
                return value.pulses[m_axis];
            }

          private:
            std::shared_ptr<StepGenerator> m_generator;
            std::size_t m_axis;
        };
    }

    StepGenerator::StepGenerator(std::unique_ptr<StepHardware> hardware,
                                 std::array<std::shared_ptr<IDigitalOutput>, 3> pins)
      : m_pins{ std::move(pins) }
      , m_hardware{ std::move(hardware) }
    {
    }
    StepGenerator::~StepGenerator()
    {
        ProgressCallback retired;
        STEP_LOCK;
        retired.swap(m_callback);
        m_hardware->shutdown();
    }
    auto StepGenerator::output(step::Axis axis) -> std::shared_ptr<IStepOutput>
    {
        const auto index{ static_cast<std::size_t>(axis) };
        if (index >= m_axes.size()) {
            return {};
        }
        return std::make_shared<StepOutput>(shared_from_this(), index);
    }
    auto StepGenerator::prepare(std::size_t axis,
                                std::span<const step::Timing> sequence,
                                std::optional<step::PulseCount> count) -> util::Result<>
    {
        if (axis >= m_axes.size() || sequence.empty() || (count && *count == 0U) ||
            (sequence.size() != 1U && (!count || *count != sequence.size()))) {
            return fail(std::errc::invalid_argument);
        }
        std::vector<Timing> timings;
        timings.reserve(sequence.size());
        for (const auto timing : sequence) {
            // Validate before rounding, including the maximum to avoid overflow.
            constexpr auto maximum{ std::chrono::nanoseconds{ std::uint64_t{ step_horizon / 2U } *
                                                              step_tick_ns } };
            if (timing.period <= std::chrono::nanoseconds::zero() || timing.period > maximum ||
                timing.high_time < std::chrono::microseconds{ 5 } ||
                timing.high_time > timing.period - std::chrono::microseconds{ 5 }) {
                return fail(std::errc::invalid_argument);
            }
            const auto period{ static_cast<std::uint32_t>((timing.period.count() + step_tick_ns - 1U) /
                                                          step_tick_ns) };
            const auto high{ static_cast<std::uint32_t>((timing.high_time.count() + step_tick_ns - 1U) /
                                                        step_tick_ns) };
            if (period - high < step_min_phase) {
                return fail(std::errc::invalid_argument);
            }
            timings.push_back({ period, high });
        }
        STEP_LOCK;
        if (m_status.state == step::State::Running || m_inCallback) {
            return fail(std::errc::device_or_resource_busy);
        }
        // Retire the old allocation after the interrupt guard is released.
        m_axes[axis].timings.swap(timings);
        m_axes[axis].requested = count;
        m_status.state = step::State::Ready;
        return {};
    }
    auto StepGenerator::clear(std::size_t axis) noexcept -> util::Result<>
    {
        STEP_LOCK;
        if (axis >= m_axes.size()) {
            return fail(std::errc::invalid_argument);
        }
        if (m_status.state == step::State::Running || m_inCallback) {
            return fail(std::errc::device_or_resource_busy);
        }
        m_axes[axis].timings.clear();
        m_status.state = std::ranges::any_of(m_axes, [](const Axis& a) { return !a.timings.empty(); })
                           ? step::State::Ready
                           : step::State::Idle;
        return {};
    }
    auto StepGenerator::fill(std::size_t index, unsigned buffer) noexcept -> void
    {
        auto& axis{ m_axes[index] };
        auto& data{ m_hardware->buffers()[index][buffer] };
        for (std::size_t i = 0; i < axis.entries; i += 2U) {
            if (axis.terminal) {
                data[i] = data[i + 1U] = step_park;
                continue;
            }
            const auto timing{ axis.timings[axis.timings.size() == 1U ? 0U : axis.generated] };
            axis.last_fall = stepAdd(axis.next_rise, timing.high);
            axis.next_rise = stepAdd(axis.next_rise, timing.period);
            ++axis.generated;
            axis.terminal = axis.requested && axis.generated == *axis.requested;
            data[i] = axis.last_fall;
            data[i + 1U] = axis.terminal ? step_park : axis.next_rise;
        }
    }
    auto StepGenerator::start(std::chrono::nanoseconds delay) noexcept -> util::Result<>
    {
        STEP_LOCK;
        if (m_status.state == step::State::Running || m_inCallback) {
            return fail(std::errc::device_or_resource_busy);
        }
        if (delay < std::chrono::microseconds{ 5 } ||
            delay.count() > std::int64_t{ step_horizon / 2U } * step_tick_ns ||
            std::ranges::all_of(m_axes, [](const Axis& a) { return a.timings.empty(); })) {
            return fail(std::errc::invalid_argument);
        }
        if (!m_hardware->reset()) {
            return fail(std::errc::io_error);
        }
        const auto first{ static_cast<std::uint32_t>((delay.count() + step_tick_ns - 1U) / step_tick_ns) };
        m_status = {};
        for (std::size_t i = 0; i < m_axes.size(); ++i) {
            auto& axis{ m_axes[i] };
            axis.generated = axis.completed_pulses = 0U;
            axis.target = 0U;
            axis.terminal = false;
            axis.finished = axis.timings.empty();
            if (axis.finished) {
                continue;
            }
            const auto longest{ std::ranges::max(axis.timings, {}, &Timing::period).period };
            // Two complete DMA buffers fit within a quarter of the timer cycle.
            // This bounds wrap comparisons and guarantees that a missed refill
            // reaches the hardware guard before any stale timestamp can recur.
            axis.entries =
              2U * std::min<std::uint32_t>(step_buffer_edges / 2U, step_horizon / (2U * longest));
            axis.next_rise = first;
            fill(i, 0U);
            fill(i, 1U);
            m_hardware->arm(i, first, axis.entries);
        }
        m_hardware->publish();
        m_status.state = step::State::Running;
        m_hardware->start(deadline(0U));
        m_notified = m_status;
        return {};
    }
    auto StepGenerator::observe(std::size_t i, const StepSample& sample) const noexcept -> Observed
    {
        const auto& axis{ m_axes[i] };
        if (axis.finished) {
            return { m_status.pulses[i], false, 0U };
        }
        const auto& channel{ sample.channels[i] };
        const unsigned blocks{ !channel.transfer_complete ? 0U : channel.target != axis.target ? 1U : 2U };
        auto edges{ axis.entries - std::min(axis.entries, channel.remaining) };
        // A compare can precede its DMA write. Confirm the phase at the pad:
        // sticky CCIF plus a past CCR alone could instead mean DMA loaded a
        // timestamp too late and the compare never happened. Never invent an
        // edge from elapsed time in that case.
        if (channel.compare_pending && channel.compare != step_park &&
            stepDistance(channel.compare, channel.tick) < step_park / 2U &&
            channel.high != ((edges & 1U) != 0U)) {
            ++edges;
        }
        const auto extra{ std::uint64_t{ blocks } * (axis.entries / 2U) + (edges + 1U) / 2U };
        const auto maximum{ std::numeric_limits<std::uint64_t>::max() };
        auto pulses{ axis.completed_pulses > maximum - extra ? maximum : axis.completed_pulses + extra };
        bool high{ (edges & 1U) != 0U };
        if (axis.requested && pulses > *axis.requested) {
            pulses = *axis.requested;
            high = false;
        }
        return { pulses, high, blocks };
    }
    auto StepGenerator::updateCounts(const StepSample& sample) noexcept -> void
    {
        for (std::size_t i = 0; i < m_axes.size(); ++i) {
            if (!m_axes[i].finished) {
                m_status.pulses[i] = observe(i, sample).pulses;
            }
        }
        if (sample.error) {
            m_status.counts_exact = false;
        }
    }
    auto StepGenerator::deadline(std::uint32_t now) const noexcept -> std::uint32_t
    {
        std::uint32_t earliest{ step_park }, latest{};
        bool needs_refill{};
        for (const auto& axis : m_axes) {
            if (axis.finished) {
                continue;
            }
            auto distance{ stepDistance(now, axis.last_fall) };
            if (distance > step_park / 2U) {
                distance = 0U;
            }
            if (!axis.terminal) {
                earliest = std::min(earliest, distance);
                needs_refill = true;
            }
            latest = std::max(latest, distance);
        }
        // Refilling may extend the deadline, but never restarts a stopped
        // counter. Finite completion occurs after every final falling edge.
        return stepAdd(now, needs_refill ? earliest : latest + step_min_phase);
    }
    auto StepGenerator::finish(step::State reason, bool send_notification) noexcept -> void
    {
        const auto sample{ m_hardware->stop() };
        // The Linux simulator can service a final virtual IRQ while advancing
        // to the stop instant. Preserve a completion already observed there.
        if (m_status.state != step::State::Running) {
            return;
        }
        updateCounts(sample);
        bool complete{ true };
        for (std::size_t i = 0; i < m_axes.size(); ++i) {
            const auto& axis{ m_axes[i] };
            if (axis.timings.empty()) {
                continue;
            }
            const auto observed{ observe(i, sample) };
            complete = complete && axis.requested && observed.pulses == *axis.requested && !observed.high;
        }
        m_status.state = sample.error || !m_status.counts_exact ? step::State::DmaError
                         : complete                             ? step::State::Completed
                                                                : reason;
        if (send_notification) {
            notify();
        }
    }
    auto StepGenerator::status() noexcept -> step::Status
    {
        STEP_LOCK;
        if (m_status.state == step::State::Running) {
            const auto sample{ m_hardware->sample() };
            if (m_status.state != step::State::Running) {
                return m_status;
            }
            updateCounts(sample);
            if (!sample.running || sample.error) {
                finish(step::State::Underrun, false);
            }
        }
        return m_status;
    }
    auto StepGenerator::stop() noexcept -> step::Status
    {
        STEP_LOCK;
        if (!m_inCallback && m_status.state == step::State::Running) {
            finish(step::State::Stopped, true);
        }
        return m_status;
    }
    auto StepGenerator::setProgressCallback(ProgressCallback callback) -> util::Result<>
    {
        STEP_LOCK;
        if (m_status.state == step::State::Running || m_inCallback) {
            return fail(std::errc::device_or_resource_busy);
        }
        m_callback.swap(callback); // The previous capture is destroyed after unlock.
        return {};
    }
    auto StepGenerator::notify() noexcept -> void
    {
        if (m_status != m_notified) {
            m_notified = m_status;
            if (m_callback && !m_inCallback) {
                m_inCallback = true;
                m_callback(m_notified);
                m_inCallback = false;
                // A read-only callback query may observe a guard completion.
                // Deliver that terminal transition once, without looping on
                // ordinary count changes while the hardware keeps running.
                if (m_status.state != step::State::Running && m_status.state != m_notified.state) {
                    notify();
                }
            }
        }
    }
    auto StepGenerator::service() noexcept -> void
    {
        STEP_LOCK;
        if (m_status.state != step::State::Running) {
            notify();
            return;
        }
        auto sample{ m_hardware->sample() };
        if (m_status.state != step::State::Running) {
            return;
        }
        if (!sample.running || sample.error) {
            finish(step::State::Underrun, true);
            return;
        }
        updateCounts(sample);
        for (std::size_t i = 0; i < m_axes.size(); ++i) {
            auto& axis{ m_axes[i] };
            if (axis.finished) {
                continue;
            }
            const auto observed{ observe(i, sample) };
            if (axis.requested && observed.pulses == *axis.requested && !observed.high) {
                m_hardware->finishAxis(i);
                axis.finished = true;
                continue;
            }
            if (observed.blocks == 0U) {
                continue;
            }
            // More than one completed block means the previous deadline was
            // missed. Never try to recover by silently restarting a stream.
            if (observed.blocks != 1U ||
                axis.completed_pulses > std::numeric_limits<std::uint64_t>::max() - axis.entries / 2U ||
                (!axis.requested &&
                 axis.generated > std::numeric_limits<std::uint64_t>::max() - axis.entries / 2U)) {
                finish(step::State::Underrun, true);
                return;
            }
            axis.completed_pulses += axis.entries / 2U;
            axis.target = sample.channels[i].target;
            m_hardware->acknowledge(i);
            fill(i, 1U - axis.target);
        }
        m_hardware->publish();
        if (std::ranges::all_of(m_axes, [](const Axis& a) { return a.finished; })) {
            finish(step::State::Completed, true);
            return;
        }
        sample = m_hardware->sample();
        if (!sample.running || sample.error) {
            finish(step::State::Underrun, true);
            return;
        }
        m_hardware->guard(deadline(sample.tick));
        notify();
    }
}

#undef STEP_LOCK
