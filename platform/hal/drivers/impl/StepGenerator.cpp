#include "hal/drivers/impl/StepGenerator.hpp"
#include "hal/drivers/impl/StepOutput.hpp"

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

namespace hal::util
{
    namespace
    {
        auto fail(std::errc error) { return std::unexpected(std::make_error_code(error)); }
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
        constexpr auto maximum{ std::chrono::nanoseconds{ std::uint64_t{ STEP_HORIZON / 2U } *
                                                          STEP_TICK_NS } };
        if (timing.period <= std::chrono::nanoseconds::zero() || timing.period > maximum ||
            timing.high_time < std::chrono::nanoseconds{ STEP_MIN_PHASE * STEP_TICK_NS } ||
            timing.high_time > timing.period - std::chrono::nanoseconds{ STEP_MIN_PHASE * STEP_TICK_NS }) {
            return fail(std::errc::invalid_argument);
        }
        const auto period{ static_cast<std::uint32_t>((timing.period.count() + STEP_TICK_NS - 1U) /
                                                      STEP_TICK_NS) };
        const auto high{ static_cast<std::uint32_t>((timing.high_time.count() + STEP_TICK_NS - 1U) /
                                                    STEP_TICK_NS) };
        if (period - high < STEP_MIN_PHASE) {
            return fail(std::errc::invalid_argument);
        }
        return Timing{ .period = period, .high = high };
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
        if (converted->period > STEP_HORIZON / axis.entries ||
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
            !convertTiming({ .period = sequence->maximumPeriod(),
                             .high_time = std::chrono::nanoseconds{ STEP_MIN_PHASE * STEP_TICK_NS } })) {
            return fail(std::errc::invalid_argument);
        }
        STEP_LOCK;
        if (m_inCallback) {
            return fail(std::errc::device_or_resource_busy);
        }
        service();
        auto& axis{ m_axes[index] };
        if (!axis.finished) {
            return fail(std::errc::device_or_resource_busy);
        }
        axis.requested = sequence->count();
        axis.sequence.swap(sequence);
        ++axis.revision;
        m_status.axes[index] = step::State::Ready;
        return {};
    }
    auto StepGenerator::scheduleCursor(std::size_t index) noexcept -> util::Result<step::ScheduleCursor>
    {
        STEP_LOCK;
        if (index >= m_axes.size()) {
            return fail(std::errc::invalid_argument);
        }
        service();
        const auto& axis{ m_axes[index] };
        if (axis.finished || !axis.sequence) {
            return fail(std::errc::operation_not_permitted);
        }
        return step::ScheduleCursor{ .first_uncommitted = axis.generated,
                                     .terminal = axis.terminal,
                                     .revision = axis.revision };
    }
    auto StepGenerator::replaceSequence(std::size_t index,
                                        step::ScheduleCursor cursor,
                                        std::shared_ptr<const step::Sequence> sequence) -> util::Result<>
    {
        if (index >= m_axes.size() || !sequence || sequence->count() <= cursor.first_uncommitted) {
            return fail(std::errc::invalid_argument);
        }
        const auto longest{ convertTiming(
          { .period = sequence->maximumPeriod(),
            .high_time = std::chrono::nanoseconds{ STEP_MIN_PHASE * STEP_TICK_NS } }) };
        if (!longest) {
            return std::unexpected(longest.error());
        }
        STEP_LOCK;
        if (m_inCallback) {
            return fail(std::errc::device_or_resource_busy);
        }
        service();
        auto& axis{ m_axes[index] };
        if (axis.finished || axis.terminal || !axis.sequence) {
            return fail(std::errc::operation_not_permitted);
        }
        if (axis.generated != cursor.first_uncommitted || axis.revision != cursor.revision) {
            return fail(std::errc::resource_unavailable_try_again);
        }
        if (longest->period > STEP_HORIZON / axis.entries) {
            return fail(std::errc::result_out_of_range);
        }
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
            if (!axis.requested || axis.generated > *axis.requested) {
                return false;
            }
            const auto count{ std::min<step::PulseCount>(axis.entries / 2U,
                                                         *axis.requested - axis.generated) };
            if (!axis.sequence->generate(axis.generated, std::span{ m_sequenceTimings }.first(count))) {
                return false;
            }
        }
        for (std::size_t i = 0; i < axis.entries; i += 2U) {
            if (axis.terminal) {
                data[i] = data[i + 1U] = STEP_PARK;
                continue;
            }
            Timing timing{};
            if (axis.sequence) {
                const auto value{ m_sequenceTimings[i / 2U] };
                const auto converted{ convertTiming(value) };
                if (!converted || value.period > axis.sequence->maximumPeriod() ||
                    converted->period > STEP_HORIZON / axis.entries) {
                    return false;
                }
                timing = *converted;
            }
            else {
                timing = axis.timings[axis.timings.size() == 1U ? 0U : axis.generated];
            }
            axis.periods[buffer][i / 2U] = timing.period;
            axis.last_fall = step_add(axis.next_rise, timing.high);
            axis.next_rise = step_add(axis.next_rise, timing.period);
            ++axis.generated;
            axis.terminal = axis.requested && axis.generated == *axis.requested;
            data[i] = axis.last_fall;
            data[i + 1U] = axis.terminal ? STEP_PARK : axis.next_rise;
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
        if (!(*m_hardware).reset()) {
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
        m_hardware->start(STEP_PARK);
        m_status.state = step::State::Running;
        m_notified = m_status;
        return {};
    }
    auto StepGenerator::startAxis(std::size_t index, std::chrono::nanoseconds delay) noexcept
      -> util::Result<>
    {
        if (index >= m_axes.size() || delay < std::chrono::nanoseconds{ STEP_MIN_PHASE * STEP_TICK_NS } ||
            delay.count() > std::int64_t{ STEP_HORIZON / 2U } * STEP_TICK_NS) {
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
        const auto first{ step_add(
          sample.tick, static_cast<std::uint32_t>((delay.count() + STEP_TICK_NS - 1U) / STEP_TICK_NS)) };
        axis.generated = axis.completed_pulses = 0U;
        axis.target = 0U;
        axis.terminal = false;
        const auto longest{ axis.sequence
                              ? convertTiming(
                                  { .period = axis.sequence->maximumPeriod(),
                                    .high_time = std::chrono::nanoseconds{ STEP_MIN_PHASE * STEP_TICK_NS } })
                                  ->period
                              : std::ranges::max(axis.timings, {}, &Timing::period).period };
        axis.entries = 2U * std::min<std::uint32_t>(STEP_BUFFER_EDGES / 2U, STEP_HORIZON / (2U * longest));
        // Short finite moves must issue a completion IRQ on their final fall.
        if (axis.requested && *axis.requested < axis.entries / 2U) {
            axis.entries = static_cast<std::uint32_t>(*axis.requested * 2U);
        }
        axis.next_rise = first;
        if (!fill(index, 0U) || !fill(index, 1U)) {
            return fail(std::errc::invalid_argument);
        }
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
    auto StepGenerator::prepareAxisBuffers(std::size_t index, std::chrono::nanoseconds delay) noexcept
      -> util::Result<>
    {
        auto& axis{ m_axes[index] };
        axis.generated = axis.completed_pulses = 0U;
        axis.target = 0U;
        axis.terminal = false;
        std::uint32_t longest{};
        if (axis.sequence) {
            const auto timing{ convertTiming(
              { .period = axis.sequence->maximumPeriod(),
                .high_time = std::chrono::nanoseconds{ STEP_MIN_PHASE * STEP_TICK_NS } }) };
            if (!timing) {
                return std::unexpected(timing.error());
            }
            longest = timing->period;
        }
        else {
            longest = std::ranges::max(axis.timings, {}, &Timing::period).period;
        }
        axis.entries = 2U * std::min<std::uint32_t>(STEP_BUFFER_EDGES / 2U, STEP_HORIZON / (2U * longest));
        if (axis.requested && *axis.requested < axis.entries / 2U) {
            axis.entries = static_cast<std::uint32_t>(*axis.requested * 2U);
        }
        axis.next_rise = static_cast<std::uint32_t>((delay.count() + STEP_TICK_NS - 1U) / STEP_TICK_NS);
        if (!fill(index, 0U) || !fill(index, 1U)) {
            return fail(std::errc::invalid_argument);
        }
        ++axis.revision;
        return {};
    }

    auto StepGenerator::rebaseAxisBuffers(std::size_t index, const StepSample& origin) noexcept -> void
    {
        auto& axis{ m_axes[index] };
        for (auto& buffer : m_hardware->buffers()[index]) {
            for (std::size_t j{}; j < axis.entries; ++j) {
                if (buffer[j] != STEP_PARK) {
                    buffer[j] = step_add(origin.tick, buffer[j]);
                }
            }
        }
        axis.next_rise = step_add(origin.tick, axis.next_rise);
        axis.last_fall = step_add(origin.tick, axis.last_fall);
    }

    auto StepGenerator::validateDelays(
      const std::array<std::optional<std::chrono::nanoseconds>, 3>& delays) noexcept -> util::Result<>
    {
        bool selected{};
        for (const auto delay : delays) {
            if (!delay) {
                continue;
            }
            selected = true;
            if (*delay < std::chrono::nanoseconds{ STEP_MIN_PHASE * STEP_TICK_NS } ||
                delay->count() > std::int64_t{ STEP_HORIZON / 2U } * STEP_TICK_NS) {
                return fail(std::errc::invalid_argument);
            }
        }
        if (!selected) {
            return fail(std::errc::invalid_argument);
        }
        return {};
    }

    auto StepGenerator::startPrepared(
      const std::array<std::optional<std::chrono::nanoseconds>, 3>& delays) noexcept -> util::Result<>
    {
        if (auto result = validateDelays(delays); !result) {
            return result;
        }
        STEP_LOCK;
        if (m_inCallback) {
            return fail(std::errc::device_or_resource_busy);
        }
        service();
        if (m_status.state != step::State::Running) {
            return fail(std::errc::operation_not_permitted);
        }
        for (std::size_t i{}; i < m_axes.size(); ++i) {
            if (!delays[i]) {
                continue;
            }
            if (!m_axes[i].finished) {
                return fail(std::errc::device_or_resource_busy);
            }
            if (m_axes[i].timings.empty() && !m_axes[i].sequence) {
                return fail(std::errc::operation_not_permitted);
            }
        }
        // Generate relative timestamps first: trajectory inversion can take
        // appreciable time, especially in a debug build, and must not consume
        // the common start lead time.
        for (std::size_t i{}; i < m_axes.size(); ++i) {
            const auto delay{ delays[i] };
            if (!delay) {
                continue;
            }
            if (auto result = prepareAxisBuffers(i, *delay); !result) {
                return result;
            }
        }
        const auto origin{ m_hardware->sample() };
        if (!origin.running || origin.error) {
            finish(step::State::Underrun, true);
            return fail(std::errc::io_error);
        }
        for (std::size_t i{}; i < m_axes.size(); ++i) {
            if (!delays[i]) {
                continue;
            }
            rebaseAxisBuffers(i, origin);
        }
        m_hardware->publish();
        return armPrepared(delays, origin);
    }

    auto StepGenerator::armPrepared(const std::array<std::optional<std::chrono::nanoseconds>, 3>& delays,
                                    const StepSample& origin) noexcept -> util::Result<>
    {
        for (std::size_t i{}; i < m_axes.size(); ++i) {
            const auto delay{ delays[i] };
            if (!delay) {
                continue;
            }
            const auto first{ step_add(
              origin.tick, static_cast<std::uint32_t>((delay->count() + STEP_TICK_NS - 1U) / STEP_TICK_NS)) };
            if (!m_hardware->arm(i, first, m_axes[i].entries)) {
                for (std::size_t j{}; j < m_axes.size(); ++j) {
                    if (delays[j]) {
                        static_cast<void>(stopAxis(j));
                    }
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
            return { .state = step::State::DmaError, .pulses = 0U, .counts_exact = false };
        }
        if (!m_inCallback) {
            service();
            if (!m_axes[index].finished) {
                const auto sample{ m_hardware->stopAxis(index) };
                updateCounts(sample);
                const auto observed{ observe(index, sample) };
                const auto requested{ m_axes[index].requested };
                m_status.axes[index] = requested && observed.pulses == *requested && !observed.high
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
            return { .pulses = m_status.pulses[i], .high = false, .blocks = 0U };
        }
        const auto& channel{ sample.channels[i] };
        unsigned blocks{};
        if (channel.transfer_complete) {
            blocks = channel.target != axis.target ? 1U : 2U;
        }
        auto edges{ axis.entries - std::min(axis.entries, channel.remaining) };
        // A compare can precede its DMA write. Confirm the phase at the pad:
        // sticky CCIF plus a past CCR alone could instead mean DMA loaded a
        // timestamp too late and the compare never happened. Never invent an
        // edge from elapsed time in that case.
        if (channel.compare_pending && channel.compare != STEP_PARK &&
            step_distance(channel.compare, channel.tick) < STEP_PARK / 2U &&
            channel.high != ((edges & 1U) != 0U)) {
            ++edges;
        }
        const auto extra{ (std::uint64_t{ blocks } * (axis.entries / 2U)) + ((edges + 1U) / 2U) };
        const auto maximum{ std::numeric_limits<std::uint64_t>::max() };
        auto pulses{ axis.completed_pulses > maximum - extra ? maximum : axis.completed_pulses + extra };
        bool high{ (edges & 1U) != 0U };
        if (axis.requested && pulses > *axis.requested) {
            pulses = *axis.requested;
            high = false;
        }
        return { .pulses = pulses, .high = high, .blocks = blocks };
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
                    m_status.periods[i] = std::chrono::nanoseconds{ std::uint64_t{ period } * STEP_TICK_NS };
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
        std::uint32_t earliest{ STEP_PARK };
        for (const auto& axis : m_axes) {
            if (axis.finished) {
                continue;
            }
            // Give the 1 ms completion monitor a further 1 ms IRQ allowance.
            // If DMA/IRQs fail, the guard still prevents stale compares at wrap.
            const auto limit{ step_add(axis.last_fall, axis.terminal ? 20'000U : 0U) };
            auto distance{ step_distance(now, limit) };
            if (distance > STEP_PARK / 2U) {
                distance = 0U;
            }
            earliest = std::min(earliest, distance);
        }
        return earliest == STEP_PARK ? STEP_PARK : step_add(now, earliest);
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
            const auto requested{ m_axes[i].requested };
            m_status.axes[i] =
              requested && observed.pulses == *requested && !observed.high ? step::State::Completed : failure;
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
        if (index >= m_axes.size()) {
            return fail(std::errc::invalid_argument);
        }
        if (m_inCallback) {
            return fail(std::errc::device_or_resource_busy);
        }
        auto& listener{ m_listeners[index] };
        if (listener.owner != nullptr && listener.owner != owner) {
            return fail(std::errc::device_or_resource_busy);
        }
        if (callback && !m_axes[index].finished) {
            return fail(std::errc::device_or_resource_busy);
        }
        listener.owner = callback ? owner : nullptr;
        listener.callback.swap(callback); // Retired capture is destroyed after unlocking.
        return {};
    }
    auto StepGenerator::setAxisProgressCallback(std::size_t index,
                                                const void* owner,
                                                IStepOutput::ProgressCallback callback) -> util::Result<>
    {
        STEP_LOCK;
        if (index >= m_axes.size()) {
            return fail(std::errc::invalid_argument);
        }
        if (m_inCallback) {
            return fail(std::errc::device_or_resource_busy);
        }
        auto& listener{ m_progressListeners[index] };
        if (listener.owner != nullptr && listener.owner != owner) {
            return fail(std::errc::device_or_resource_busy);
        }
        if (callback && !m_axes[index].finished) {
            return fail(std::errc::device_or_resource_busy);
        }
        listener.owner = callback ? owner : nullptr;
        listener.callback.swap(callback);
        return {};
    }
    auto StepGenerator::axisStatus(std::size_t index) const noexcept -> step::AxisStatus
    {
        return { .state = m_status.axes[index],
                 .pulses = m_status.pulses[index],
                 .counts_exact = m_status.counts_exact,
                 .period = m_status.periods[index] };
    }
    auto StepGenerator::notify() noexcept -> void
    {
        if (m_inCallback || m_status == m_notified) {
            return;
        }
        m_notified = m_status;
        m_inCallback = true;
        if (m_callback) {
            m_callback(m_notified);
        }
        for (std::size_t i = 0; i < m_axes.size(); ++i) {
            auto& progress{ m_progressListeners[i] };
            const auto current{ axisStatus(i) };
            if (progress.active && current != progress.notified) {
                progress.notified = current;
                if (m_axes[i].finished) {
                    progress.active = false;
                }
                if (progress.callback) {
                    progress.callback(current);
                }
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
                axis.completed_pulses > std::numeric_limits<std::uint64_t>::max() - (axis.entries / 2U) ||
                (!axis.requested &&
                 axis.generated > std::numeric_limits<std::uint64_t>::max() - (axis.entries / 2U))) {
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
