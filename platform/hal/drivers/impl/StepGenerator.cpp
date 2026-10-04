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
            ~StepOutput() override
            {
                static_cast<void>(m_generator->setAxisProgressCallback(m_axis, this, {}));
                static_cast<void>(m_generator->setCompletionCallback(m_axis, this, {}));
            }
            auto setProgressCallback(ProgressCallback callback) -> util::Result<> override
            {
                return m_generator->setAxisProgressCallback(m_axis, this, std::move(callback));
            }
            auto setCompletionCallback(CompletionCallback callback) -> util::Result<> override
            {
                return m_generator->setCompletionCallback(m_axis, this, std::move(callback));
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
            auto prepareSequence(std::shared_ptr<const step::Sequence> sequence) -> util::Result<> override
            {
                return m_generator->prepareSequence(m_axis, std::move(sequence));
            }
            auto scheduleCursor() noexcept -> util::Result<step::ScheduleCursor> override
            {
                return m_generator->scheduleCursor(m_axis);
            }
            auto replaceSequence(step::ScheduleCursor cursor, std::shared_ptr<const step::Sequence> sequence)
              -> util::Result<> override
            {
                return m_generator->replaceSequence(m_axis, cursor, std::move(sequence));
            }
            auto start(std::chrono::nanoseconds delay) noexcept -> util::Result<> override
            {
                return m_generator->startAxis(m_axis, delay);
            }
            auto stop() noexcept -> step::AxisStatus override { return m_generator->stopAxis(m_axis); }
            auto status() noexcept -> step::AxisStatus override
            {
                const auto group{ m_generator->status() };
                return {
                    group.axes[m_axis], group.pulses[m_axis], group.counts_exact, group.periods[m_axis]
                };
            }
            auto updateTiming(step::Timing timing) noexcept -> util::Result<step::PulseCount> override
            {
                return m_generator->updateTiming(m_axis, timing);
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
        m_hardware->stopService();
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
    auto StepGenerator::convertTiming(step::Timing timing) noexcept -> util::Result<Timing>
    {
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
        return Timing{ period, high };
    }
    auto StepGenerator::prepare(std::size_t index,
                                std::span<const step::Timing> sequence,
                                std::optional<step::PulseCount> count) -> util::Result<>
    {
        if (index >= m_axes.size() || sequence.empty() || (count && *count == 0U) ||
            (sequence.size() != 1U && (!count || *count != sequence.size()))) {
            return fail(std::errc::invalid_argument);
        }
        std::vector<Timing> timings;
        std::shared_ptr<const step::Sequence> retired;
        timings.reserve(sequence.size());
        for (const auto timing : sequence) {
            const auto converted{ convertTiming(timing) };
            if (!converted) {
                return std::unexpected(converted.error());
            }
            timings.push_back(*converted);
        }
        STEP_LOCK;
        if (m_inCallback) {
            return fail(std::errc::device_or_resource_busy);
        }
        service();
        if (!m_axes[index].finished) {
            return fail(std::errc::device_or_resource_busy);
        }
        m_axes[index].timings.swap(timings); // Free retired allocation after unlock.
        m_axes[index].sequence.swap(retired);
        ++m_axes[index].revision;
        m_axes[index].requested = count;
        m_status.axes[index] = step::State::Ready;
        return {};
    }
    auto StepGenerator::clear(std::size_t index) noexcept -> util::Result<>
    {
        std::shared_ptr<const step::Sequence> retired;
        STEP_LOCK;
        if (index >= m_axes.size()) {
            return fail(std::errc::invalid_argument);
        }
        if (m_inCallback) {
            return fail(std::errc::device_or_resource_busy);
        }
        service();
        if (!m_axes[index].finished) {
            return fail(std::errc::device_or_resource_busy);
        }
        m_axes[index].timings.clear();
        m_axes[index].sequence.swap(retired);
        ++m_axes[index].revision;
        m_status.axes[index] = step::State::Idle;
        return {};
    }
    auto StepGenerator::updateTiming(std::size_t index, step::Timing timing) noexcept
      -> util::Result<step::PulseCount>
    {
        const auto converted{ convertTiming(timing) };
        if (!converted || index >= m_axes.size()) {
            return fail(std::errc::invalid_argument);
        }
        STEP_LOCK;
        if (m_inCallback) {
            return fail(std::errc::device_or_resource_busy);
        }
        service();
        auto& axis{ m_axes[index] };
        if (m_status.state != step::State::Running || axis.finished || axis.terminal || axis.sequence ||
            axis.timings.size() != 1U) {
            return fail(std::errc::operation_not_permitted);
        }
        // NDTR stays fixed during a move. Both buffers must still fit inside
        // the modular comparison horizon after a live change to a longer period.
        if (converted->period > step_horizon / axis.entries ||
            axis.generated == std::numeric_limits<step::PulseCount>::max()) {
            return fail(std::errc::result_out_of_range);
        }
        axis.timings[0] = *converted;
        return axis.generated + 1U;
    }
    auto StepGenerator::prepareSequence(std::size_t index, std::shared_ptr<const step::Sequence> sequence)
      -> util::Result<>
    {
        if (index >= m_axes.size() || !sequence || sequence->count() == 0U ||
            !convertTiming({ sequence->maximumPeriod(), std::chrono::microseconds{ 5 } }))
            return fail(std::errc::invalid_argument);
        STEP_LOCK;
        if (m_inCallback)
            return fail(std::errc::device_or_resource_busy);
        service();
        auto& axis{ m_axes[index] };
        if (!axis.finished)
            return fail(std::errc::device_or_resource_busy);
        axis.requested = sequence->count();
        axis.sequence.swap(sequence);
        ++axis.revision;
        m_status.axes[index] = step::State::Ready;
        return {};
    }
    auto StepGenerator::scheduleCursor(std::size_t index) noexcept -> util::Result<step::ScheduleCursor>
    {
        STEP_LOCK;
        if (index >= m_axes.size())
            return fail(std::errc::invalid_argument);
        service();
        const auto& axis{ m_axes[index] };
        if (axis.finished || !axis.sequence)
            return fail(std::errc::operation_not_permitted);
        return step::ScheduleCursor{ axis.generated, axis.terminal, axis.revision };
    }
    auto StepGenerator::replaceSequence(std::size_t index,
                                        step::ScheduleCursor cursor,
                                        std::shared_ptr<const step::Sequence> sequence) -> util::Result<>
    {
        if (index >= m_axes.size() || !sequence || sequence->count() <= cursor.first_uncommitted)
            return fail(std::errc::invalid_argument);
        const auto longest{ convertTiming({ sequence->maximumPeriod(), std::chrono::microseconds{ 5 } }) };
        if (!longest)
            return std::unexpected(longest.error());
        STEP_LOCK;
        if (m_inCallback)
            return fail(std::errc::device_or_resource_busy);
        service();
        auto& axis{ m_axes[index] };
        if (axis.finished || axis.terminal || !axis.sequence)
            return fail(std::errc::operation_not_permitted);
        if (axis.generated != cursor.first_uncommitted || axis.revision != cursor.revision)
            return fail(std::errc::resource_unavailable_try_again);
        if (longest->period > step_horizon / axis.entries)
            return fail(std::errc::result_out_of_range);
        axis.requested = sequence->count();
        axis.sequence.swap(sequence);
        ++axis.revision;
        return {};
    }
    auto StepGenerator::fill(std::size_t index, unsigned buffer) noexcept -> bool
    {
        auto& axis{ m_axes[index] };
        auto& data{ m_hardware->buffers()[index][buffer] };
        if (axis.sequence && !axis.terminal) {
            const auto count{ std::min<step::PulseCount>(axis.entries / 2U,
                                                         *axis.requested - axis.generated) };
            if (!axis.sequence->generate(axis.generated, std::span{ m_sequenceTimings }.first(count)))
                return false;
        }
        for (std::size_t i = 0; i < axis.entries; i += 2U) {
            if (axis.terminal) {
                data[i] = data[i + 1U] = step_park;
                continue;
            }
            Timing timing{};
            if (axis.sequence) {
                const auto value{ m_sequenceTimings[i / 2U] };
                const auto converted{ convertTiming(value) };
                if (!converted || value.period > axis.sequence->maximumPeriod() ||
                    converted->period > step_horizon / axis.entries)
                    return false;
                timing = *converted;
            }
            else
                timing = axis.timings[axis.timings.size() == 1U ? 0U : axis.generated];
            axis.periods[buffer][i / 2U] = timing.period;
            axis.last_fall = stepAdd(axis.next_rise, timing.high);
            axis.next_rise = stepAdd(axis.next_rise, timing.period);
            ++axis.generated;
            axis.terminal = axis.requested && axis.generated == *axis.requested;
            data[i] = axis.last_fall;
            data[i + 1U] = axis.terminal ? step_park : axis.next_rise;
        }
        return true;
    }
    auto StepGenerator::start() noexcept -> util::Result<>
    {
        STEP_LOCK;
        if (m_inCallback) {
            return fail(std::errc::device_or_resource_busy);
        }
        if (m_status.state == step::State::Running) {
            service();
            return m_status.state == step::State::Running ? util::Result<>{} : fail(std::errc::io_error);
        }
        if (m_status.state == step::State::DmaError || m_status.state == step::State::Underrun) {
            return fail(std::errc::io_error);
        }
        if (!m_hardware->reset()) {
            return fail(std::errc::io_error);
        }
        m_status = {};
        for (std::size_t i = 0; i < m_axes.size(); ++i) {
            m_axes[i].finished = true;
            m_listeners[i].notified = true;
            m_progressListeners[i].active = false;
            m_status.axes[i] =
              m_axes[i].timings.empty() && !m_axes[i].sequence ? step::State::Idle : step::State::Ready;
        }
        m_hardware->start(step_park);
        m_status.state = step::State::Running;
        m_notified = m_status;
        return {};
    }
    auto StepGenerator::startAxis(std::size_t index, std::chrono::nanoseconds delay) noexcept
      -> util::Result<>
    {
        if (index >= m_axes.size() || delay < std::chrono::microseconds{ 5 } ||
            delay.count() > std::int64_t{ step_horizon / 2U } * step_tick_ns) {
            return fail(std::errc::invalid_argument);
        }
        STEP_LOCK;
        if (m_inCallback) {
            return fail(std::errc::device_or_resource_busy);
        }
        service();
        auto& axis{ m_axes[index] };
        if (m_status.state != step::State::Running || (axis.timings.empty() && !axis.sequence)) {
            return fail(std::errc::operation_not_permitted);
        }
        if (!axis.finished) {
            return fail(std::errc::device_or_resource_busy);
        }
        const auto sample{ m_hardware->sample() };
        if (!sample.running || sample.error) {
            finish(step::State::Underrun, true);
            return fail(std::errc::io_error);
        }
        const auto first{ stepAdd(
          sample.tick, static_cast<std::uint32_t>((delay.count() + step_tick_ns - 1U) / step_tick_ns)) };
        axis.generated = axis.completed_pulses = 0U;
        axis.target = 0U;
        axis.terminal = false;
        const auto longest{
            axis.sequence
              ? convertTiming({ axis.sequence->maximumPeriod(), std::chrono::microseconds{ 5 } })->period
              : std::ranges::max(axis.timings, {}, &Timing::period).period
        };
        axis.entries = 2U * std::min<std::uint32_t>(step_buffer_edges / 2U, step_horizon / (2U * longest));
        // Short finite moves must issue a completion IRQ on their final fall.
        if (axis.requested && *axis.requested < axis.entries / 2U) {
            axis.entries = static_cast<std::uint32_t>(*axis.requested * 2U);
        }
        axis.next_rise = first;
        if (!fill(index, 0U) || !fill(index, 1U))
            return fail(std::errc::invalid_argument);
        ++axis.revision;
        m_hardware->publish();
        // First compare is still parked until arm validates its remaining lead.
        if (!m_hardware->arm(index, first, axis.entries)) {
            service();
            return fail(std::errc::timed_out);
        }
        axis.finished = false;
        m_listeners[index].notified = false;
        m_status.pulses[index] = 0U;
        m_status.periods[index] = {};
        m_status.axes[index] = step::State::Running;
        // Establish this run's accounting before any progress/fault callback.
        auto& listener{ m_progressListeners[index] };
        listener.active = true;
        listener.notified = axisStatus(index);
        if (listener.callback) {
            m_inCallback = true;
            listener.callback(listener.notified);
            m_inCallback = false;
        }
        const auto armed{ m_hardware->sample() };
        if (!armed.running || armed.error) {
            finish(step::State::Underrun, true);
            return fail(std::errc::io_error);
        }
        guard(armed.tick);
        // Preserve the generator-wide callback's no-start notification contract.
        m_notified = m_status;
        return {};
    }
    auto StepGenerator::startPrepared(
      const std::array<std::optional<std::chrono::nanoseconds>, 3>& delays) noexcept -> util::Result<>
    {
        bool selected{};
        for (const auto delay : delays) {
            if (!delay)
                continue;
            selected = true;
            if (*delay < std::chrono::microseconds{ 5 } ||
                delay->count() > std::int64_t{ step_horizon / 2U } * step_tick_ns)
                return fail(std::errc::invalid_argument);
        }
        if (!selected)
            return fail(std::errc::invalid_argument);
        STEP_LOCK;
        if (m_inCallback)
            return fail(std::errc::device_or_resource_busy);
        service();
        if (m_status.state != step::State::Running)
            return fail(std::errc::operation_not_permitted);
        for (std::size_t i{}; i < m_axes.size(); ++i) {
            if (!delays[i])
                continue;
            if (!m_axes[i].finished)
                return fail(std::errc::device_or_resource_busy);
            if (m_axes[i].timings.empty() && !m_axes[i].sequence)
                return fail(std::errc::operation_not_permitted);
        }
        // Generate relative timestamps first: trajectory inversion can take
        // appreciable time, especially in a debug build, and must not consume
        // the common start lead time.
        for (std::size_t i{}; i < m_axes.size(); ++i) {
            if (!delays[i])
                continue;
            auto& axis{ m_axes[i] };
            axis.generated = axis.completed_pulses = 0U;
            axis.target = 0U;
            axis.terminal = false;
            const auto longest{
                axis.sequence
                  ? convertTiming({ axis.sequence->maximumPeriod(), std::chrono::microseconds{ 5 } })->period
                  : std::ranges::max(axis.timings, {}, &Timing::period).period
            };
            axis.entries =
              2U * std::min<std::uint32_t>(step_buffer_edges / 2U, step_horizon / (2U * longest));
            if (axis.requested && *axis.requested < axis.entries / 2U)
                axis.entries = static_cast<std::uint32_t>(*axis.requested * 2U);
            axis.next_rise =
              static_cast<std::uint32_t>((delays[i]->count() + step_tick_ns - 1U) / step_tick_ns);
            if (!fill(i, 0U) || !fill(i, 1U))
                return fail(std::errc::invalid_argument);
            ++axis.revision;
        }
        const auto origin{ m_hardware->sample() };
        if (!origin.running || origin.error) {
            finish(step::State::Underrun, true);
            return fail(std::errc::io_error);
        }
        for (std::size_t i{}; i < m_axes.size(); ++i) {
            if (!delays[i])
                continue;
            auto& axis{ m_axes[i] };
            for (auto& buffer : m_hardware->buffers()[i]) {
                for (std::size_t j{}; j < axis.entries; ++j) {
                    if (buffer[j] != step_park)
                        buffer[j] = stepAdd(origin.tick, buffer[j]);
                }
            }
            axis.next_rise = stepAdd(origin.tick, axis.next_rise);
            axis.last_fall = stepAdd(origin.tick, axis.last_fall);
        }
        m_hardware->publish();
        for (std::size_t i{}; i < m_axes.size(); ++i) {
            if (!delays[i])
                continue;
            const auto first{ stepAdd(
              origin.tick,
              static_cast<std::uint32_t>((delays[i]->count() + step_tick_ns - 1U) / step_tick_ns)) };
            if (!m_hardware->arm(i, first, m_axes[i].entries)) {
                for (std::size_t j{}; j < m_axes.size(); ++j) {
                    if (delays[j])
                        static_cast<void>(stopAxis(j));
                }
                return fail(std::errc::timed_out);
            }
            m_axes[i].finished = false;
            m_listeners[i].notified = false;
            m_status.pulses[i] = 0U;
            m_status.periods[i] = {};
            m_status.axes[i] = step::State::Running;
            auto& listener{ m_progressListeners[i] };
            listener.active = true;
            listener.notified = axisStatus(i);
            if (listener.callback) {
                m_inCallback = true;
                listener.callback(listener.notified);
                m_inCallback = false;
            }
        }
        const auto armed{ m_hardware->sample() };
        if (!armed.running || armed.error) {
            finish(step::State::Underrun, true);
            return fail(std::errc::io_error);
        }
        guard(armed.tick);
        m_notified = m_status;
        return {};
    }

    auto StepGenerator::stopAxis(std::size_t index) noexcept -> step::AxisStatus
    {
        STEP_LOCK;
        if (index >= m_axes.size()) {
            return { step::State::DmaError, 0U, false };
        }
        if (!m_inCallback) {
            service();
            if (!m_axes[index].finished) {
                const auto sample{ m_hardware->stopAxis(index) };
                updateCounts(sample);
                const auto observed{ observe(index, sample) };
                m_status.axes[index] =
                  m_axes[index].requested && observed.pulses == *m_axes[index].requested && !observed.high
                    ? step::State::Completed
                    : step::State::Stopped;
                m_axes[index].finished = true;
                m_status.periods[index] = {};
                if (!sample.running || sample.error) {
                    finish(step::State::Underrun, true);
                }
                else {
                    guard(sample.tick);
                    notify();
                }
            }
        }
        return axisStatus(index);
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
                const auto pulses{ observe(i, sample).pulses };
                const auto& axis{ m_axes[i] };
                if (pulses != m_status.pulses[i] && pulses != 0U) {
                    // Read before refilling: a just-completed buffer can still
                    // own the latest pulse until the next rising edge. Retain
                    // its period when subsequent samples see that same count.
                    const auto period{ axis.periods[((pulses - 1U) / (axis.entries / 2U)) % 2U]
                                                   [(pulses - 1U) % (axis.entries / 2U)] };
                    m_status.periods[i] = std::chrono::nanoseconds{ std::uint64_t{ period } * step_tick_ns };
                }
                m_status.pulses[i] = pulses;
            }
        }
        if (sample.error) {
            m_status.counts_exact = false;
        }
    }
    auto StepGenerator::deadline(std::uint32_t now) const noexcept -> std::uint32_t
    {
        std::uint32_t earliest{ step_park };
        for (const auto& axis : m_axes) {
            if (axis.finished) {
                continue;
            }
            // Give the 1 ms completion monitor a further 1 ms IRQ allowance.
            // If DMA/IRQs fail, the guard still prevents stale compares at wrap.
            const auto limit{ stepAdd(axis.last_fall, axis.terminal ? 20'000U : 0U) };
            auto distance{ stepDistance(now, limit) };
            if (distance > step_park / 2U) {
                distance = 0U;
            }
            earliest = std::min(earliest, distance);
        }
        return earliest == step_park ? step_park : stepAdd(now, earliest);
    }
    auto StepGenerator::guard(std::uint32_t now) noexcept -> void
    {
        m_hardware->completionWatch(
          std::ranges::any_of(m_axes, [](const Axis& axis) { return !axis.finished && axis.terminal; }));
        m_hardware->guard(deadline(now));
    }
    auto StepGenerator::finish(step::State reason, bool send_notification) noexcept -> void
    {
        const auto sample{ m_hardware->stop() };
        if (m_status.state != step::State::Running) {
            return;
        }
        updateCounts(sample);
        const auto failure{ sample.error || !m_status.counts_exact ? step::State::DmaError : reason };
        for (std::size_t i = 0; i < m_axes.size(); ++i) {
            if (m_axes[i].finished) {
                continue;
            }
            const auto observed{ observe(i, sample) };
            m_status.axes[i] =
              m_axes[i].requested && observed.pulses == *m_axes[i].requested && !observed.high
                ? step::State::Completed
                : failure;
            m_axes[i].finished = true;
            m_status.periods[i] = {};
        }
        m_status.state = failure;
        if (send_notification) {
            notify();
        }
    }
    auto StepGenerator::status() noexcept -> step::Status
    {
        STEP_LOCK;
        if (!m_inCallback) {
            service();
        }
        return m_status;
    }
    auto StepGenerator::stop() noexcept -> step::Status
    {
        STEP_LOCK;
        if (!m_inCallback) {
            if (m_status.state == step::State::Running) {
                finish(step::State::Stopped, true);
            }
            else {
                m_status.state = step::State::Stopped; // Explicit fault acknowledgement.
            }
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
    auto StepGenerator::setCompletionCallback(std::size_t index,
                                              const void* owner,
                                              IStepOutput::CompletionCallback callback) -> util::Result<>
    {
        STEP_LOCK;
        if (index >= m_axes.size())
            return fail(std::errc::invalid_argument);
        if (m_inCallback)
            return fail(std::errc::device_or_resource_busy);
        auto& listener{ m_listeners[index] };
        if (listener.owner != nullptr && listener.owner != owner) {
            return fail(std::errc::device_or_resource_busy);
        }
        if (callback && !m_axes[index].finished)
            return fail(std::errc::device_or_resource_busy);
        listener.owner = callback ? owner : nullptr;
        listener.callback.swap(callback); // Retired capture is destroyed after unlocking.
        return {};
    }
    auto StepGenerator::setAxisProgressCallback(std::size_t index,
                                                const void* owner,
                                                IStepOutput::ProgressCallback callback) -> util::Result<>
    {
        STEP_LOCK;
        if (index >= m_axes.size())
            return fail(std::errc::invalid_argument);
        if (m_inCallback)
            return fail(std::errc::device_or_resource_busy);
        auto& listener{ m_progressListeners[index] };
        if (listener.owner != nullptr && listener.owner != owner)
            return fail(std::errc::device_or_resource_busy);
        if (callback && !m_axes[index].finished)
            return fail(std::errc::device_or_resource_busy);
        listener.owner = callback ? owner : nullptr;
        listener.callback.swap(callback);
        return {};
    }
    auto StepGenerator::axisStatus(std::size_t index) const noexcept -> step::AxisStatus
    {
        return {
            m_status.axes[index], m_status.pulses[index], m_status.counts_exact, m_status.periods[index]
        };
    }
    auto StepGenerator::notify() noexcept -> void
    {
        if (m_inCallback || m_status == m_notified)
            return;
        m_notified = m_status;
        m_inCallback = true;
        if (m_callback)
            m_callback(m_notified);
        for (std::size_t i = 0; i < m_axes.size(); ++i) {
            auto& progress{ m_progressListeners[i] };
            const auto current{ axisStatus(i) };
            if (progress.active && current != progress.notified) {
                progress.notified = current;
                if (m_axes[i].finished)
                    progress.active = false;
                if (progress.callback)
                    progress.callback(current);
            }
            auto& listener{ m_listeners[i] };
            if (!listener.notified && m_axes[i].finished) {
                listener.notified = true;
                if (listener.callback) {
                    listener.callback(current);
                }
            }
        }
        m_inCallback = false;
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
                m_status.axes[i] = step::State::Completed;
                m_status.periods[i] = {};
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
            if (!fill(i, 1U - axis.target)) {
                finish(step::State::DmaError, true);
                return;
            }
        }
        m_hardware->publish();
        sample = m_hardware->sample();
        if (!sample.running || sample.error) {
            finish(step::State::Underrun, true);
            return;
        }
        guard(sample.tick);
        notify();
    }
}

#undef STEP_LOCK
