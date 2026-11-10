#include "hal/drivers/impl/StepGenerator.hpp"
#include "hal/drivers/impl/StepOutput.hpp"
#include "hal/drivers/util/StepTrace.hpp"
#include "profiling/Trace.h"

#if defined(HAL_PLATFORM_STM32)
#include "hal/stm32/InterruptGuard.hpp"
#define STEP_LOCK                                                                                            \
    const stm32::InterruptGuard lock {}
#elif defined(HAL_PLATFORM_LINUX)
#include "simulation/Client.hpp"
#define STEP_LOCK                                                                                            \
    const simulation::client::Transaction transaction{};                                                     \
    std::unique_lock lock{ m_mutex, std::defer_lock };                                                       \
    if (!simulation::client::connected())                                                                    \
    lock.lock()
#else
#define STEP_LOCK                                                                                            \
    const std::scoped_lock lock { m_mutex }
#endif

#include <algorithm>
#include <experimental/scope>
#include <limits>
#include <utility>

#if defined(GIGA_MOTION_BENCH)
extern "C" std::uint32_t giga_motion_bench_buffer_edges;
extern "C" volatile std::uint32_t giga_motion_bench_refill_ticks;
extern "C" std::uint32_t giga_motion_bench_refill_index[3];
extern "C" volatile std::uint32_t giga_motion_bench_worst_refill_cycles;
extern "C" volatile std::uint32_t giga_motion_bench_worst_budget_ticks;
// Debugger-readable screening counters: completed axis, scratch capacity,
// changed revision, expanded queued prefix, turnover/TC, target mismatch.
extern "C" {
std::uint32_t giga_motion_bench_splice_retries[6]{};
}
// Last startup per axis: origin, armed compare, DMA first word, replacement
// tick, old compare, new compare, new first word, DMA reload performed.
extern "C" {
std::array<std::array<std::uint32_t, 8>, 3> giga_motion_bench_startup{};
}
#endif

extern "C" {
unsigned motion_debug_fault_line{};
}
namespace hal::util
{
    struct StepGenerator::AxisStart
    {
        std::shared_ptr<const step::Sequence> sequence{};
        std::optional<step::PulseCount> requested{};
        std::uint64_t revision{};
        std::uint32_t entries{}, first{}, next{}, last{};
        step::PulseCount generated{};
        bool terminal{};
        std::chrono::nanoseconds maximum{};
    };
    struct StepGenerator::PreparedStart
    {
        std::array<std::optional<step::PulseCount>, 3> requested{};
        std::array<std::uint64_t, 3> revisions{};
        std::array<std::uint32_t, 3> sizes{};
        std::uint32_t capacity{ 2 };
    };
    struct StepGenerator::RefillState
    {
        std::shared_ptr<const step::Sequence> sequence{};
        std::optional<step::PulseCount> requested{};
        std::uint64_t revision{}, generated{}, original_generated{};
        std::uint32_t entries{}, next{}, last{};
        unsigned target{};
        bool terminal{};
    };
    struct StepGenerator::RefillWorkspace
    {
        std::array<step::Timing, STEP_BUFFER_EDGES / 2> timings;
        std::array<std::uint32_t, STEP_BUFFER_EDGES> edges{};
        std::array<std::uint32_t, STEP_BUFFER_EDGES / 2> periods{};
    };
    struct StepGenerator::SpliceWorkspace
    {
        struct Channel
        {
            // Two extra halves permit two DMA turnovers during preparation.
            struct Tail
            {
                std::uint64_t generated;
                std::uint32_t next, last;
                bool terminal;
            };
            std::array<std::span<std::uint32_t>, 4> edges, periods;
            std::array<std::uint32_t, 4> firsts{};
            std::array<Tail, 4> tails{};
            unsigned shift{};
            std::shared_ptr<const step::Sequence> previous;
            std::uint64_t revision{}, base{}, generated{};
            std::uint32_t entries{}, origin{}, next{}, last{}, first{};
            unsigned target{};
            bool terminal{}, unchanged{};
            std::optional<std::int64_t> prefix;
        };
        std::array<Channel, 3> axes;
        static constexpr std::size_t WORDS_PER_ENTRY{ 18 }; // 3 axes × (4 edges + 4 half-sized periods).
        std::uint32_t capacity;
        std::vector<std::uint32_t> words;
        std::vector<step::Timing> timings;
        explicit SpliceWorkspace(std::uint32_t entries)
          : capacity{ entries } // Count constructors: braces would create two-element initializer lists.
                                // NOLINTNEXTLINE(protomate-brace-initialization)
          , words(std::size_t(entries) * WORDS_PER_ENTRY)
          // NOLINTNEXTLINE(protomate-brace-initialization)
          , timings(std::size_t(entries) * 2)
        {
            // Scratch buffers are not DMA-owned. Size them for the actual
            // prepared window, instead of reserving the maximum legacy window.
            auto remaining{ std::span(words) };
            for (auto& axis : axes) {
                for (auto& edge : axis.edges) {
                    edge = remaining.first(entries);
                    remaining = remaining.subspan(entries);
                }
                for (auto& period : axis.periods) {
                    period = remaining.first(entries / 2);
                    remaining = remaining.subspan(entries / 2);
                }
            }
        }
    };
    namespace
    {
        auto fail(std::errc error) { return std::unexpected(std::make_error_code(error)); }
        enum class RetryReason : std::uint8_t
        {
            Completed,
            Capacity,
            Revision,
            Prefix,
            Turnover,
            Target
        };
        constexpr unsigned SPLICE_LEAD_TICKS{ 20'000 }, REFILL_LEAD_TICKS{ 1'000 },
          TERMINAL_GUARD_TICKS{ 20'000 };
        constexpr unsigned GUARD_FAILURE_TAG{ 0x20000U };
        auto retry(RetryReason reason)
        {
#if defined(GIGA_MOTION_BENCH)
            ++giga_motion_bench_splice_retries[std::to_underlying(reason)];
#else
            static_cast<void>(reason);
#endif
            return fail(std::errc::resource_unavailable_try_again);
        }
        bool valid_extent(const step::Sequence& sequence)
        {
            const auto end{ sequence.endTickOffset() };
            return std::cmp_less(sequence.initialOffset(), STEP_HORIZON) &&
                   (!end || (*end > -std::int64_t(STEP_HORIZON) && std::cmp_less(*end, STEP_HORIZON)));
        }
        std::optional<std::uint32_t> end_tick(const step::Sequence* sequence, std::uint32_t first)
        {
            if (!sequence) {
                return {};
            }
            const auto end{ sequence->endTickOffset() };
            if (!end) {
                return {};
            }
            const auto offset{ *end };
            const auto value{ (std::int64_t(first) + offset + STEP_PARK) % STEP_PARK };
            return std::uint32_t(value);
        }
    }

    auto StepGenerator::reserveScratch(std::uint32_t entries) noexcept -> util::Result<>
    {
        if (m_splice && m_splice->capacity >= entries) {
            return {};
        }
        // Do not temporarily retain both allocations when growing. Every user
        // holds m_splicing and releases its temporary schedule references before
        // returning. Active DMA buffers and installed sequences live elsewhere.
        m_splice.reset();
        try {
            m_splice = std::make_unique<SpliceWorkspace>(entries);
            return {};
        } catch (...) {
            return fail(std::errc::not_enough_memory);
        }
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
        ProgressCallback retired{};
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
        std::vector<Timing> timings{};
        std::shared_ptr<const step::Sequence> retired{};
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
    auto StepGenerator::timeline() noexcept -> util::Result<Timeline>
    {
        STEP_LOCK;
        service();
        if (m_status.state != step::State::Running) {
            return fail(std::errc::operation_not_permitted);
        }
        Timeline result{ .tick = m_hardware->sample().tick };
        for (unsigned i{ 0 }; i < 3; ++i) {
            result.first_rise[i] = m_axes[i].first_rise;
        }
        return result;
    }

    auto StepGenerator::captureSpliceAxis(unsigned i, const step::TimedSequence& sequence) -> util::Result<>
    {
        auto& work{ *m_splice };
        const auto& a{ m_axes[i] };
        // An axis can complete after the owner's status snapshot but
        // before this transaction. Let it refresh the participating set;
        // this is not a failure of the remaining axes' installed paths.
        if (a.finished) {
            return retry(RetryReason::Completed);
        }
        if (a.refill_pending) {
            return retry(RetryReason::Turnover);
        }
        if (!a.sequence) {
            return fail(std::errc::operation_not_permitted);
        }
        if (a.entries > work.capacity) {
            return retry(RetryReason::Capacity);
        }
        auto& w{ work.axes[i] };
        w.previous = a.sequence;
        w.revision = a.revision;
        w.base = a.completed_pulses;
        w.entries = a.entries;
        w.target = a.target;
        if (sequence.timeBase() < a.sequence->timeBase() ||
            sequence.timeBase() - a.sequence->timeBase() >= STEP_HORIZON) {
            return fail(std::errc::invalid_argument);
        }
        w.origin = step_add(a.first_rise, std::uint32_t(sequence.timeBase() - a.sequence->timeBase()));
        w.next = a.next_rise;
        w.terminal = a.terminal;
        w.generated = a.generated;
        for (unsigned half{ 0 }; half < 2; ++half) {
            std::copy_n(m_hardware->buffers()[i][a.target ^ half].begin(), a.entries, w.edges[half].begin());
            std::copy_n(a.periods[a.target ^ half].begin(), a.entries / 2, w.periods[half].begin());
        }
        return {};
    }

    auto StepGenerator::certifySpliceAxis(unsigned i, const step::TimedSequence& sequence, bool unchanged)
      -> bool
    {
        auto& work{ *m_splice };
        auto& w{ work.axes[i] };
        w.prefix = sequence.commonPrefix(*w.previous);
        if (!unchanged || w.terminal || !w.prefix || *w.prefix < 0 ||
            std::cmp_less(*w.prefix, step_distance(w.origin, w.next)) || sequence.count() < w.generated) {
            return false;
        }
        // Reserve one complete refill beyond the current snapshot, because DMA
        // can advance before the final publication validation.
        const auto lookahead{ w.generated + (w.entries / 2) };
        if (lookahead >= sequence.count()) {
            return false;
        }
        const auto edge{ sequence.risingTick(lookahead) };
        return edge && std::cmp_less_equal(*edge, *w.prefix);
    }

    auto StepGenerator::copiedSpliceIntervals(unsigned i, const step::TimedSequence& sequence) const
      -> std::uint32_t
    {
        const auto& w{ m_splice->axes[i] };
        const auto count{ std::min<std::uint64_t>(std::uint64_t(w.entries) * 2, sequence.count() - w.base) };
        std::uint32_t copied{};
        if (w.prefix && *w.prefix >= 0) {
            while (copied < std::min<std::uint64_t>(count, w.entries)) {
                const auto half{ copied / (w.entries / 2) };
                const auto n{ (copied % (w.entries / 2)) * 2 };
                const auto fall{ w.edges[half][n] };
                const auto rise{ w.edges[half][n + 1] };
                if (fall == STEP_PARK || rise == STEP_PARK ||
                    std::cmp_greater(step_distance(w.origin, fall), *w.prefix) ||
                    std::cmp_greater(step_distance(w.origin, rise), *w.prefix)) {
                    break;
                }
                ++copied;
            }
        }
        return copied;
    }

    auto StepGenerator::stageSpliceAxis(unsigned i, const step::TimedSequence& sequence) -> util::Result<>
    {
        auto& work{ *m_splice };
        auto& w{ work.axes[i] };
        STEP_TRACE(mark(step_trace::Axis0 + 3 * i));
        if (sequence.count() < w.base) {
            return fail(std::errc::invalid_argument);
        }
        const auto count{ std::min<std::uint64_t>(std::uint64_t(w.entries) * 2, sequence.count() - w.base) };
        const auto start{ count ? sequence.risingTick(w.base) : util::Result<std::uint32_t>{ 0 } };
        if (!start || *start >= STEP_HORIZON) {
            return fail(std::errc::invalid_argument);
        }
        // Reuse complete intervals certified identical to the previous
        // schedule. The period crossing the certificate boundary is always
        // regenerated; a copied rising edge cannot fix the next interval.
        const auto copied{ copiedSpliceIntervals(i, sequence) };
        STEP_TRACE(work(i, unsigned(count - copied), copied, w.entries));
        if (!sequence.generate(w.base + copied, std::span(work.timings).subspan(copied, count - copied))) {
            return fail(std::errc::invalid_argument);
        }
        STEP_TRACE(mark(step_trace::Generated0 + 3 * i));
        w.first = w.next = count ? step_add(w.origin, *start) : STEP_PARK;
        w.last = count ? w.first : w.origin;
        w.generated = w.base;
        w.terminal = count == 0;
        for (unsigned half{ 0 }; half < 4; ++half) {
            const auto buffer{ half };
            w.firsts[half] = w.terminal ? STEP_PARK : w.next;
            for (unsigned n{ 0 }; n < w.entries; n += 2) {
                auto& rise{ w.edges[buffer][n + 1] };
                auto& fall{ w.edges[buffer][n] };
                if (w.terminal) {
                    rise = fall = STEP_PARK;
                    continue;
                }
                if (w.generated - w.base < copied) {
                    w.last = fall;
                    w.next = rise;
                    ++w.generated;
                    continue;
                }
                const auto timing{ convertTiming(work.timings[w.generated - w.base]) };
                if (!timing || timing->period > STEP_HORIZON / w.entries) {
                    return fail(std::errc::invalid_argument);
                }
                w.periods[buffer][n / 2] = timing->period;
                w.last = step_add(w.next, timing->high);
                w.next = step_add(w.next, timing->period);
                w.terminal = ++w.generated == sequence.count();
                fall = w.last;
                rise = w.terminal ? STEP_PARK : w.next;
            }
            w.tails[half] = {
                .generated = w.generated, .next = w.next, .last = w.last, .terminal = w.terminal
            };
        }
        STEP_TRACE(mark(step_trace::Staged0 + 3 * i));
        return {};
    }

    auto StepGenerator::validSplicePrefix(unsigned i, const step::TimedSequence& sequence) const -> bool
    {
        const auto& a{ m_axes[i] };
        const auto& w{ m_splice->axes[i] };
        return !a.terminal && w.prefix && *w.prefix >= 0 &&
               !std::cmp_less(*w.prefix, step_distance(w.origin, a.next_rise)) &&
               sequence.count() >= a.generated;
    }

    namespace
    {
        struct MutableEdge
        {
            std::uint32_t boundary;
            bool operator()(std::uint32_t tick) const
            {
                const auto distance{ step_distance(boundary, tick) };
                return tick == STEP_PARK || (distance > 0 && distance < STEP_HORIZON);
            }
        };
    }
    auto StepGenerator::validateSpliceAxis(unsigned i,
                                           const step::TimedSequence& sequence,
                                           const StepSample& sample,
                                           std::uint32_t preserve_until,
                                           std::uint32_t& compare,
                                           bool& restart) -> util::Result<>
    {
        auto& work{ *m_splice };
        const MutableEdge mutable_edge{ preserve_until };
        const auto& a{ m_axes[i] };
        auto& w{ work.axes[i] };
        const auto& channel{ sample.channels[i] };
        if (a.finished || a.revision != w.revision) {
            return retry(RetryReason::Revision);
        }
        if (a.refill_pending) {
            return retry(RetryReason::Turnover);
        }
        if (w.unchanged) {
            // DMA may have advanced while the immutable replacement
            // was prepared. No DMA word changes on this path: certify
            // the CURRENT queued prefix, rather than requiring the
            // original snapshot's buffer to remain active.
            return validSplicePrefix(i, sequence) ? util::Result<>{} : retry(RetryReason::Prefix);
        }
        if (a.completed_pulses < w.base || a.completed_pulses - w.base > w.entries ||
            channel.transfer_complete || channel.remaining == 0 || channel.remaining > w.entries) {
            return retry(RetryReason::Turnover);
        }
        w.shift = unsigned((a.completed_pulses - w.base) / (w.entries / 2));
        if (channel.target != (w.target ^ (w.shift & 1U))) {
            return retry(RetryReason::Target);
        }
        const auto used{ w.entries - channel.remaining };
        compare = used ? w.edges[w.shift][used - 1] : w.firsts[w.shift];
        if (compare != channel.compare && (!mutable_edge(compare) || !mutable_edge(channel.compare))) {
            return fail(std::errc::invalid_argument);
        }
        // Direct-mode DMA has already fetched the NEXT memory word,
        // even before the first compare. Publishing RAM cannot replace
        // that word (RM0399 16.3.3). In a protected LOW interval we can
        // rebase the prepared buffers to the next pulse, then re-arm
        // LOW and reload the FIFO without losing emitted-step counts.
        const auto prefetched{ m_hardware->buffers()[i][a.target][used] };
        if (prefetched != w.edges[w.shift][used]) {
            restart = !channel.high && used % 2U == 0 && w.shift < 2 && mutable_edge(compare) &&
                      mutable_edge(channel.compare);
            if (!restart) {
                return retry(RetryReason::Target);
            }
        }
        for (unsigned b{ 0 }; b < 2; ++b) {
            for (unsigned n{ 0 }; n < w.entries; ++n) {
                const auto old{ m_hardware->buffers()[i][a.target ^ b][n] };
                const auto next{ w.edges[w.shift + b][n] };
                if (old != next && (!mutable_edge(old) || !mutable_edge(next))) {
                    return fail(std::errc::invalid_argument);
                }
            }
        }
        return {};
    }

    auto StepGenerator::publishSpliceAxis(unsigned i,
                                          const std::shared_ptr<const step::TimedSequence>& sequence,
                                          const StepSample& sample,
                                          std::uint32_t compare,
                                          bool restart) -> util::Result<>
    {
        auto& work{ *m_splice };
        auto& a{ m_axes[i] };
        const auto& w{ work.axes[i] };
        a.first_rise = w.origin;
        if (w.unchanged) {
            a.sequence = sequence;
            a.requested = sequence->count();
            a.end_tick = end_tick(a.sequence.get(), a.first_rise);
            ++a.revision;
            return {};
        }
        if (restart) {
            // This channel is LOW and its next edge is beyond the
            // protected frontier. Park DMA before rebasing its buffer
            // indices; published past entries may then be reused.
            const auto parked{ m_hardware->stopAxis(i) };
            if (parked.channels[i].high || !parked.running || parked.error) {
                finish(step::State::Underrun, true);
                return fail(std::errc::io_error);
            }
            const auto consumed{ (w.shift * w.entries) + w.entries - sample.channels[i].remaining };
#if defined(GIGA_MOTION_BENCH)
            if (w.base == 0 && consumed == 0) {
                auto& trace{ giga_motion_bench_startup[i] };
                trace[3] = sample.tick;
                trace[4] = sample.channels[i].compare;
                trace[5] = compare;
                trace[6] = w.edges[0][0];
                trace[7] = 1;
            }
#endif
            a.completed_pulses = w.base + (consumed / 2);
            a.generated = a.completed_pulses;
            a.target = 0;
            a.next_rise = compare;
            a.terminal = a.generated == sequence->count();
            for (unsigned b{ 0 }; b < 2; ++b) {
                for (unsigned n{ 0 }; n < w.entries; n += 2) {
                    auto& data{ m_hardware->buffers()[i][b] };
                    if (a.terminal) {
                        data[n] = data[n + 1] = STEP_PARK;
                        continue;
                    }
                    const auto source{ consumed + (b * w.entries) + n };
                    const auto half{ source / w.entries };
                    const auto offset{ source % w.entries };
                    data[n] = w.edges[half][offset];
                    data[n + 1] = w.edges[half][offset + 1];
                    a.periods[b][n / 2] = w.periods[half][offset / 2];
                    a.last_fall = data[n];
                    a.next_rise = step_add(a.next_rise, a.periods[b][n / 2]);
                    a.terminal = ++a.generated == sequence->count();
                }
            }
            m_hardware->publish();
            if (!m_hardware->arm(i, compare, a.entries)) {
                STEP_TRACE(failure(step_trace::Arm, step_distance(sample.tick, compare)));
                finish(step::State::Underrun, true);
                return fail(std::errc::timed_out);
            }
            a.sequence = sequence;
            a.requested = sequence->count();
            a.end_tick = end_tick(a.sequence.get(), a.first_rise);
            ++a.revision;
            return {};
        }
        for (unsigned b{ 0 }; b < 2; ++b) {
            for (unsigned n{ 0 }; n < w.entries; ++n) {
                if (m_hardware->buffers()[i][a.target ^ b][n] != w.edges[w.shift + b][n]) {
                    m_hardware->buffers()[i][a.target ^ b][n] = w.edges[w.shift + b][n];
                }
            }
        }
        m_hardware->publish();
        if (compare != sample.channels[i].compare) {
            m_hardware->retarget(i, compare);
        }
        a.sequence = sequence; // work.previous keeps the retired object alive.
        a.requested = sequence->count();
        const auto& tail{ w.tails[w.shift + 1] };
        a.generated = tail.generated;
        a.next_rise = tail.next;
        a.last_fall = tail.last;
        a.terminal = tail.terminal;
        a.end_tick = end_tick(a.sequence.get(), a.first_rise);
        for (unsigned b{ 0 }; b < 2; ++b) {
            std::copy_n(w.periods[w.shift + b].begin(), w.entries / 2, a.periods[a.target ^ b].begin());
        }
        ++a.revision;
        return {};
    }

    auto StepGenerator::captureSplice(
      const std::array<std::shared_ptr<const step::TimedSequence>, 3>& sequences) -> util::Result<>
    {
        STEP_LOCK;
        if (m_inCallback) {
            return fail(std::errc::device_or_resource_busy);
        }
        service();
        if (m_status.state != step::State::Running) {
            return fail(std::errc::io_error);
        }
        bool selected{};
        for (unsigned i{ 0 }; i < 3; ++i) {
            if (!sequences[i]) {
                continue;
            }
            selected = true;
            if (auto result{ captureSpliceAxis(i, *sequences[i]) }; !result) {
                return result;
            }
        }
        if (!selected) {
            return fail(std::errc::invalid_argument);
        }

        return {};
    }

    auto StepGenerator::prepareSplice(
      const std::array<std::shared_ptr<const step::TimedSequence>, 3>& sequences) -> util::Result<>
    {
        auto& work{ *m_splice };
        // Use the pointer-only path only when every participating axis is
        // covered. Mixing it with another axis's expensive buffer generation
        // can let DMA refill beyond the certified prefix before publication,
        // repeatedly rejecting an otherwise renewable multi-axis command.
        bool unchanged{ true };
        for (unsigned i{ 0 }; i < 3; ++i) {
            if (!sequences[i]) {
                continue;
            }
            unchanged = certifySpliceAxis(i, *sequences[i], unchanged);
        }
        STEP_TRACE(mark(step_trace::Certificate));
        // Expensive evaluation and conversion take place with interrupts enabled.
        for (unsigned i{ 0 }; i < 3; ++i) {
            if (!sequences[i]) {
                continue;
            }
            work.axes[i].unchanged = unchanged;
            if (!unchanged) {
                if (auto result{ stageSpliceAxis(i, *sequences[i]) }; !result) {
                    return result;
                }
            }
        }

        return {};
    }

    auto StepGenerator::commitSplice(
      const std::array<std::shared_ptr<const step::TimedSequence>, 3>& sequences,
      std::uint32_t preserve_until) -> util::Result<>
    {

        STEP_LOCK;
        StepSample sample{};
        // DMA continues during service and register sampling. Both TC and
        // a changed target mean the snapshot needs accounting, even when
        // TC was not observed. Resample this staged transaction rather
        // than rebuilding it. Three attempts are a hard bound, and the
        // staging window/deadline checks below still apply.
        for (unsigned attempt{ 0 }; attempt < 3; ++attempt) {
            service();
            if (m_status.state != step::State::Running) {
                return fail(std::errc::io_error);
            }
            sample = m_hardware->sample();
            bool pending{};
            for (unsigned i{ 0 }; i < 3; ++i) {
                pending |= sequences[i] && (sample.channels[i].transfer_complete ||
                                            sample.channels[i].target != m_axes[i].target);
            }
            if (!pending) {
                break;
            }
        }
        const auto lead{ step_distance(sample.tick, preserve_until) };
        STEP_TRACE(mark(step_trace::FinalSample));
        if (lead < SPLICE_LEAD_TICKS || lead >= STEP_HORIZON) {
            STEP_TRACE(failure(step_trace::LeadTime, lead));
            return fail(std::errc::timed_out);
        }
        std::array<std::uint32_t, 3> compares{};
        std::array<bool, 3> restart{};
        // Validate ALL axes before changing ANY DMA-owned word.
        for (unsigned i{ 0 }; i < 3; ++i) {
            if (!sequences[i]) {
                continue;
            }
            if (auto result{
                  validateSpliceAxis(i, *sequences[i], sample, preserve_until, compares[i], restart[i]) };
                !result) {
                return result;
            }
        }
        m_hardware->spliceWindow(preserve_until);
        for (unsigned i{ 0 }; i < 3; ++i) {
            if (!sequences[i]) {
                continue;
            }
            if (auto result{ publishSpliceAxis(i, sequences[i], sample, compares[i], restart[i]) }; !result) {
                return result;
            }
        }
        m_hardware->spliceWindow({});
        STEP_TRACE(mark(step_trace::Published));
        const auto after{ m_hardware->sample() };
        if (step_distance(after.tick, preserve_until) >= STEP_HORIZON || after.error) {
            // A violated publication budget is a local hardware fault, not
            // a rejected command with an intact old trajectory.
            finish(step::State::Underrun, true);
            return fail(std::errc::io_error);
        }
        guard(after.tick);

        return {};
    }

    auto StepGenerator::splice(const std::array<std::shared_ptr<const step::TimedSequence>, 3>& sequences,
                               std::uint32_t preserve_until) -> util::Result<>
    {
        STEP_TRACE(mark(step_trace::Enter));
        preparePending();
        STEP_TRACE(mark(step_trace::RefilledBefore));
        if (m_splicing.test_and_set()) {
            return fail(std::errc::device_or_resource_busy);
        }
        const std::experimental::scope_exit unlock{ [this] { m_splicing.clear(); } };
        if (!m_hardware->supportsRetarget()) {
            return fail(std::errc::not_supported);
        }
        for (const auto& sequence : sequences) {
            if (sequence && !valid_extent(*sequence)) {
                return fail(std::errc::invalid_argument);
            }
        }
        std::uint32_t entries{ 2 };
        {
            STEP_LOCK;
            for (unsigned i{ 0 }; i < 3; ++i) {
                if (sequences[i]) {
                    entries = std::max(entries, m_axes[i].entries);
                }
            }
        }
        if (const auto reserved{ reserveScratch(entries) }; !reserved) {
            return reserved;
        }
        auto& work{ *m_splice };
        const std::experimental::scope_exit retire{ [&work] {
            for (auto& axis : work.axes) {
                axis.previous.reset();
            }
        } };
        // References from the preceding call are retired in thread context.
        for (auto& a : work.axes) {
            a.previous.reset();
        }
        if (auto result{ captureSplice(sequences) }; !result) {
            return result;
        }
        STEP_TRACE(mark(step_trace::Snapshot));
        if (auto result{ prepareSplice(sequences) }; !result) {
            return result;
        }
        preparePending(); // DMA may have consumed a half during replacement calculation.
        STEP_TRACE(mark(step_trace::RefilledAfter));
        return commitSplice(sequences, preserve_until);
    }

    auto StepGenerator::clear(std::size_t index) noexcept -> util::Result<>
    {
        std::unique_ptr<SpliceWorkspace> retired_workspace{};
        std::shared_ptr<const step::Sequence> retired{};
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
        if (std::ranges::none_of(m_axes, [](const auto& axis) { return bool(axis.sequence); }) &&
            !m_splicing.test_and_set()) {
            retired_workspace = std::move(m_splice);
            m_splicing.clear();
        }
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
        if (index >= m_axes.size() || !sequence || !valid_extent(*sequence) ||
            (sequence->count() == 0U && !sequence->endTickOffset()) ||
            !convertTiming({ .period = sequence->maximumPeriod(),
                             .high_time = std::chrono::nanoseconds{ STEP_MIN_PHASE * STEP_TICK_NS } })) {
            return fail(std::errc::invalid_argument);
        }
        if (sequence->endTickOffset()) {
            if (m_splicing.test_and_set()) {
                return fail(std::errc::device_or_resource_busy);
            }
            const std::experimental::scope_exit unlock{ [this] { m_splicing.clear(); } };
            const auto entries{ 2U * std::clamp<std::uint32_t>(
                                       sequence->bufferPulses(), 1U, STEP_BUFFER_EDGES / 2U) };
            if (const auto reserved{ reserveScratch(entries) }; !reserved) {
                return reserved;
            }
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
        // Sequence metadata is immutable. Read its bound once per refill,
        // rather than making one virtual call for every generated pulse.
        const auto maximum_period{ axis.sequence ? axis.sequence->maximumPeriod()
                                                 : std::chrono::nanoseconds{} };
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
        for (std::size_t i{ 0 }; i < axis.entries; i += 2U) {
            if (axis.terminal) {
                data[i] = data[i + 1U] = STEP_PARK;
                continue;
            }
            Timing timing{};
            if (axis.sequence) {
                const auto value{ m_sequenceTimings[i / 2U] };
                const auto converted{ convertTiming(value) };
                if (!converted || value.period > maximum_period ||
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
    auto StepGenerator::setRefillNotification(RefillNotification callback) -> util::Result<>
    {
        // Fixed allocation at registration, not during motion or under PRIMASK.
        if (callback && !m_refill) {
            m_refill = std::make_unique<RefillWorkspace>();
        }
        STEP_LOCK;
        if (m_inCallback || std::ranges::any_of(m_axes, [](const auto& a) { return !a.finished; })) {
            return fail(std::errc::device_or_resource_busy);
        }
        m_refillNotification.swap(callback);
        return {};
    }
    bool StepGenerator::captureRefill(std::size_t i, RefillState& state) noexcept
    {
        auto& timings{ m_refill->timings };
        STEP_LOCK;
        service();
        auto& a{ m_axes[i] };
        if (!m_refillNotification || !a.refill_pending || a.finished) {
            return false;
        }
        state.sequence = a.sequence;
        state.requested = a.requested;
        state.revision = a.revision;
        state.generated = state.original_generated = a.generated;
        state.entries = a.entries;
        state.next = a.next_rise;
        state.last = a.last_fall;
        state.target = a.target;
        state.terminal = a.terminal;
        if (!state.sequence) {
            for (unsigned p{ 0 }; p < state.entries / 2; ++p) {
                if (state.requested && state.generated + p >= *state.requested) {
                    break;
                }
                const auto t{ a.timings[a.timings.size() == 1 ? 0 : state.generated + p] };
                timings[p] = { .period = std::chrono::nanoseconds(std::uint64_t(t.period) * STEP_TICK_NS),
                               .high_time = std::chrono::nanoseconds(std::uint64_t(t.high) * STEP_TICK_NS) };
            }
        }
        return true;
    }
    bool StepGenerator::calculateRefill(RefillState& state) noexcept
    {
        auto& timings{ m_refill->timings };
        auto& edges{ m_refill->edges };
        auto& periods{ m_refill->periods };
        bool valid{ true };
        if (state.sequence && !state.terminal) {
            if (!state.requested || *state.requested < state.generated) {
                return false;
            }
            const auto count{ std::min<step::PulseCount>(state.entries / 2,
                                                         *state.requested - state.generated) };
            valid = bool(state.sequence->generate(state.generated, std::span(timings).first(count)));
        }
        const auto maximum{ state.sequence ? state.sequence->maximumPeriod()
                                           : std::chrono::nanoseconds::max() };
        for (unsigned n{ 0 }; valid && n < state.entries; n += 2) {
            if (state.terminal) {
                edges[n] = edges[n + 1] = STEP_PARK;
                periods[n / 2] = 0;
                continue;
            }
            const auto t{ convertTiming(timings[n / 2]) };
            if (!t || timings[n / 2].period > maximum || t->period > STEP_HORIZON / state.entries) {
                valid = false;
                break;
            }
            periods[n / 2] = t->period;
            state.last = step_add(state.next, t->high);
            state.next = step_add(state.next, t->period);
            state.terminal = state.requested && ++state.generated == *state.requested;
            if (!state.requested) {
                ++state.generated;
            }
            edges[n] = state.last;
            edges[n + 1] = state.terminal ? STEP_PARK : state.next;
        }
        return valid;
    }
    bool StepGenerator::commitRefill(std::size_t i, const RefillState& state, bool valid) noexcept
    {
        const auto& edges{ m_refill->edges };
        const auto& periods{ m_refill->periods };
        STEP_LOCK;
        service();
        auto& a{ m_axes[i] };
        if (a.finished || !a.refill_pending || a.revision != state.revision || a.target != state.target ||
            a.generated != state.original_generated) {
            return true; // A stop/replacement won while the worker calculated.
        }
        const auto before{ m_hardware->sample() };
        const auto lead{ step_distance(before.tick, a.last_fall) };
        // Copy is bounded; the old guard remains armed throughout it.
        if (!valid || !before.running || before.error || lead < REFILL_LEAD_TICKS || lead >= STEP_HORIZON) {
            finish(step::State::Underrun, true);
            return false;
        }
        std::copy_n(edges.begin(), state.entries, m_hardware->buffers()[i][1U - state.target].begin());
        std::copy_n(periods.begin(), state.entries / 2, a.periods[1U - state.target].begin());
        m_hardware->publish();
        const auto after{ m_hardware->sample() };
        if (!after.running || after.error || after.channels[i].target != state.target ||
            after.channels[i].transfer_complete) {
            finish(step::State::Underrun, true);
            return false;
        }
        a.generated = state.generated;
        a.next_rise = state.next;
        a.last_fall = state.last;
        a.terminal = state.terminal;
        a.refill_pending = false;
#if defined(GIGA_MOTION_BENCH)
        // Complete refill transaction: notification delay, computation
        // and publication, measured on the common 10 MHz timeline.
        const auto elapsed{ step_distance(a.refill_tick, after.tick) * 24U };
        if (!giga_motion_bench_worst_refill_cycles ||
            std::uint64_t(elapsed) * giga_motion_bench_worst_budget_ticks >
              std::uint64_t(giga_motion_bench_worst_refill_cycles) * a.refill_budget) {
            giga_motion_bench_worst_refill_cycles = elapsed;
            giga_motion_bench_worst_budget_ticks = a.refill_budget;
        }
#endif
        guard(after.tick);
        return true;
    }
    void StepGenerator::preparePending() noexcept
    {
        if (m_preparing.test_and_set()) {
            return;
        }
        const std::experimental::scope_exit unlock{ [this] { m_preparing.clear(); } };
        if (!m_refill) {
            return;
        }
        // Only this worker stages data; DMA sees it after revision-checked publication.
        for (std::size_t i{}; i < m_axes.size(); ++i) {
            RefillState state{};
            if (!captureRefill(i, state)) {
                continue;
            }
            const bool valid{ calculateRefill(state) };
            if (!commitRefill(i, state, valid)) {
                return;
            }
        }
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
        for (std::size_t i{ 0 }; i < m_axes.size(); ++i) {
            m_axes[i].finished = true;
            m_axes[i].refill_pending = false;
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
    auto StepGenerator::captureAxisStart(std::size_t index, AxisStart& start) noexcept -> util::Result<>
    {
        const auto mask{ static_cast<std::uint8_t>(1U << index) };
        STEP_LOCK;
        if (m_inCallback || m_splicing.test() || (m_starting.load() & mask) != 0U) {
            return fail(std::errc::device_or_resource_busy);
        }
        service();
        const auto& axis{ m_axes[index] };
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
        const auto longest{ axis.sequence
                              ? convertTiming(
                                  { .period = axis.sequence->maximumPeriod(),
                                    .high_time = std::chrono::nanoseconds{ STEP_MIN_PHASE * STEP_TICK_NS } })
                                  ->period
                              : std::ranges::max(axis.timings, {}, &Timing::period).period };
        const auto window{
#if defined(GIGA_MOTION_BENCH)
            std::clamp<std::uint32_t>(giga_motion_bench_buffer_edges, 4U, STEP_BUFFER_EDGES) / 2U
#else
            axis.sequence
              ? std::clamp<std::uint32_t>(axis.sequence->bufferPulses(), 1U, STEP_BUFFER_EDGES / 2U)
              : STEP_BUFFER_EDGES / 2U
#endif
        };
        start.entries = 2U * std::min<std::uint32_t>(window, STEP_HORIZON / (2U * longest));
        start.requested = axis.requested;
        start.sequence = axis.sequence;
        start.revision = axis.revision;
        if (start.requested && *start.requested != 0U && *start.requested < start.entries / 2U &&
            (!start.sequence || !start.sequence->endTickOffset())) {
            start.entries = static_cast<std::uint32_t>(*start.requested * 2U);
        }
        m_starting.fetch_or(mask);
        return {};
    }
    auto StepGenerator::writeStartBatch(std::size_t index,
                                        AxisStart& start,
                                        std::size_t half,
                                        std::uint32_t& entry,
                                        std::span<const step::Timing> timings) noexcept -> util::Result<>
    {
        const auto count{ timings.size() };
        STEP_LOCK;
        const auto& axis{ m_axes[index] };
        if (m_status.state != step::State::Running || !axis.finished || axis.revision != start.revision) {
            return fail(std::errc::operation_canceled);
        }
        for (std::size_t pulse{}; pulse < count; ++pulse, entry += 2U) {
            auto& buffer{ m_hardware->buffers()[index][half] };
            if (start.terminal) {
                buffer[entry] = buffer[entry + 1U] = STEP_PARK;
                m_axes[index].periods[half][entry / 2U] = 0U;
                continue;
            }
            const auto timing{ start.sequence
                                 ? convertTiming(timings[pulse])
                                 : util::Result<Timing>{
                                     axis.timings[axis.timings.size() == 1U ? 0U : start.generated] } };
            if (!timing || (start.sequence && timings[pulse].period > start.maximum) ||
                timing->period > STEP_HORIZON / start.entries) {
                return fail(std::errc::invalid_argument);
            }
            start.last = step_add(start.next, timing->high);
            start.next = step_add(start.next, timing->period);
            ++start.generated;
            start.terminal = start.requested && start.generated == *start.requested;
            m_axes[index].periods[half][entry / 2U] = timing->period;
            buffer[entry] = start.last;
            buffer[entry + 1U] = start.terminal ? STEP_PARK : start.next;
        }
        return {};
    }
    auto StepGenerator::prepareAxisStart(std::size_t index, AxisStart& start) noexcept -> util::Result<>
    {
        // Keep generated batches on the stack and outside interrupt-disabled sections.
        constexpr std::size_t preparation_batch_pulses{ 8U };
        std::array<step::Timing, preparation_batch_pulses> timings{};
        for (std::size_t half{}; half < 2U; ++half) {
            for (std::uint32_t entry{}; entry < start.entries;) {
                const auto count{ std::min<std::size_t>(timings.size(), (start.entries - entry) / 2U) };
                const auto active{ start.requested
                                     ? std::min<step::PulseCount>(count, *start.requested - start.generated)
                                     : count };
                if (start.sequence && active != 0U &&
                    !start.sequence->generate(start.generated, std::span{ timings }.first(active))) {
                    return fail(std::errc::invalid_argument);
                }
                if (auto result{
                      writeStartBatch(index, start, half, entry, std::span{ timings }.first(count)) };
                    !result) {
                    return result;
                }
                preparePending();
            }
        }
        return {};
    }
    auto StepGenerator::publishAxisStart(std::size_t index,
                                         const AxisStart& start,
                                         std::chrono::nanoseconds delay) noexcept -> util::Result<>
    {
        STEP_LOCK;
        service();
        auto& axis{ m_axes[index] };
        if (m_status.state != step::State::Running || !axis.finished || axis.revision != start.revision) {
            return fail(std::errc::operation_canceled);
        }
        const auto origin{ m_hardware->sample() };
        if (!origin.running || origin.error) {
            finish(step::State::Underrun, true);
            return fail(std::errc::io_error);
        }
        for (auto& buffer : m_hardware->buffers()[index]) {
            for (std::uint32_t entry{}; entry < start.entries; ++entry) {
                if (buffer[entry] != STEP_PARK) {
                    buffer[entry] = step_add(origin.tick, buffer[entry]);
                }
            }
        }
        axis.entries = start.entries;
        axis.generated = start.generated;
        axis.completed_pulses = 0U;
        axis.refill_pending = false;
        axis.target = 0U;
        axis.terminal = start.terminal;
        axis.next_rise = step_add(origin.tick, start.next);
        axis.first_rise = step_add(origin.tick, start.first);
        axis.last_fall = step_add(origin.tick, start.last);
        axis.end_tick = end_tick(start.sequence.get(), axis.first_rise);
        ++axis.revision;
        m_hardware->publish();
        std::array<std::optional<std::chrono::nanoseconds>, 3> delays{};
        delays[index] = delay;
        return armPrepared(delays, origin);
    }
    auto StepGenerator::startAxis(std::size_t index, std::chrono::nanoseconds delay) noexcept
      -> util::Result<>
    {
        if (index >= m_axes.size() || delay < std::chrono::nanoseconds{ STEP_MIN_PHASE * STEP_TICK_NS } ||
            delay.count() > std::int64_t{ STEP_HORIZON / 2U } * STEP_TICK_NS) {
            return fail(std::errc::invalid_argument);
        }
        AxisStart start{};
        if (auto result{ captureAxisStart(index, start) }; !result) {
            return result;
        }
        const auto mask{ static_cast<std::uint8_t>(1U << index) };
        const std::experimental::scope_exit retire{ [this, mask] {
            m_starting.fetch_and(static_cast<std::uint8_t>(~mask));
        } };
        start.first = static_cast<std::uint32_t>((delay.count() + STEP_TICK_NS - 1U) / STEP_TICK_NS);
        start.next = step_add(start.first, start.sequence ? start.sequence->initialOffset() : 0U);
        start.last = start.first;
        start.terminal = start.requested && *start.requested == 0U;
        start.maximum = start.sequence ? start.sequence->maximumPeriod() : std::chrono::nanoseconds::max();
        if (auto result{ prepareAxisStart(index, start) }; !result) {
            return result;
        }
        return publishAxisStart(index, start, delay);
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

    auto StepGenerator::capturePrepared(const std::array<std::optional<std::chrono::nanoseconds>, 3>& delays,
                                        PreparedStart& start) noexcept -> util::Result<>
    {
        STEP_LOCK;
        if (m_inCallback || m_starting.load() != 0U) {
            return fail(std::errc::device_or_resource_busy);
        }
        service();
        if (m_status.state != step::State::Running) {
            return fail(std::errc::operation_not_permitted);
        }
        for (unsigned i{ 0 }; i < 3; ++i) {
            if (!delays[i]) {
                continue;
            }
            const auto& a{ m_axes[i] };
            if (!a.finished) {
                return fail(std::errc::device_or_resource_busy);
            }
            if (!a.sequence && a.timings.empty()) {
                return fail(std::errc::operation_not_permitted);
            }
            start.revisions[i] = a.revision;
            start.requested[i] = a.requested;
            const auto maximum{
                a.sequence
                  ? convertTiming({ .period = a.sequence->maximumPeriod(),
                                    .high_time = std::chrono::nanoseconds{ STEP_MIN_PHASE * STEP_TICK_NS } })
                  : util::Result<Timing>{ std::ranges::max(a.timings, {}, &Timing::period) }
            };
            if (!maximum) {
                return std::unexpected(maximum.error());
            }
            const auto window{ a.sequence ? std::clamp<std::uint32_t>(
                                              a.sequence->bufferPulses(), 1U, STEP_BUFFER_EDGES / 2U)
                                          : STEP_BUFFER_EDGES / 2U };
            start.sizes[i] = 2 * std::min<std::uint32_t>(window, STEP_HORIZON / (2 * maximum->period));
#if defined(GIGA_MOTION_BENCH)
            start.sizes[i] =
              2 *
              std::min(std::clamp<std::uint32_t>(giga_motion_bench_buffer_edges, 4U, STEP_BUFFER_EDGES) / 2U,
                       STEP_HORIZON / (2 * maximum->period));
#endif
            const auto requested{ start.requested[i] };
            if (requested && *requested && *requested < start.sizes[i] / 2 &&
                (!a.sequence || !a.sequence->endTickOffset())) {
                start.sizes[i] = std::uint32_t(*requested * 2);
            }
            start.capacity = std::max(start.capacity, start.sizes[i]);
        }
        return {};
    }
    auto StepGenerator::buildStartHalf(std::size_t i, const PreparedStart& start, unsigned half) noexcept
      -> util::Result<>
    {
        auto& work{ *m_splice };
        auto& w{ work.axes[i] };
        const auto maximum{ w.previous ? w.previous->maximumPeriod() : std::chrono::nanoseconds::max() };
        const auto requested{ start.requested[i] };
        const auto count{ requested ? std::min<step::PulseCount>(w.entries / 2, *requested - w.generated)
                                    : w.entries / 2 };
        if (w.previous) {
            if (!w.previous->generate(w.generated, std::span(work.timings).first(count))) {
                return fail(std::errc::invalid_argument);
            }
        }
        else {
            STEP_LOCK;
            const auto& a{ m_axes[i] };
            if (!a.finished || a.revision != start.revisions[i]) {
                return fail(std::errc::operation_canceled);
            }
            for (unsigned n{ 0 }; n < count; ++n) {
                const auto t{ a.timings[a.timings.size() == 1 ? 0 : w.generated + n] };
                work.timings[n] = { .period =
                                      std::chrono::nanoseconds(std::uint64_t(t.period) * STEP_TICK_NS),
                                    .high_time =
                                      std::chrono::nanoseconds(std::uint64_t(t.high) * STEP_TICK_NS) };
            }
        }
        for (unsigned n{ 0 }; n < w.entries; n += 2) {
            if (w.terminal) {
                w.edges[half][n] = w.edges[half][n + 1] = STEP_PARK;
                w.periods[half][n / 2] = 0;
                continue;
            }
            const auto t{ convertTiming(work.timings[n / 2]) };
            if (!t || work.timings[n / 2].period > maximum || t->period > STEP_HORIZON / w.entries) {
                return fail(std::errc::invalid_argument);
            }
            w.periods[half][n / 2] = t->period;
            w.last = step_add(w.next, t->high);
            w.next = step_add(w.next, t->period);
            ++w.generated;
            w.terminal = requested && w.generated == *requested;
            w.edges[half][n] = w.last;
            w.edges[half][n + 1] = w.terminal ? STEP_PARK : w.next;
        }
        return {};
    }
    auto StepGenerator::prepareStartAxis(std::size_t i,
                                         const PreparedStart& start,
                                         std::chrono::nanoseconds delay) noexcept -> util::Result<>
    {
        auto& work{ *m_splice };
        auto& w{ work.axes[i] };
        w.entries = start.sizes[i];
        w.generated = 0;
        w.first = std::uint32_t((delay.count() + STEP_TICK_NS - 1) / STEP_TICK_NS);
        const auto requested{ start.requested[i] };
        w.terminal = requested && *requested == 0;
        {
            STEP_LOCK;
            const auto& a{ m_axes[i] };
            if (!a.finished || a.revision != start.revisions[i]) {
                return fail(std::errc::operation_canceled);
            }
            w.previous = a.sequence;
        }
        w.next = step_add(w.first, w.previous ? w.previous->initialOffset() : 0);
        w.last = w.next;
        for (unsigned half{ 0 }; half < 2; ++half) {
            if (const auto result{ buildStartHalf(i, start, half) }; !result) {
                return result;
            }
            // Preserve service of already-running independent channels between halves.
            preparePending();
        }
        return {};
    }
    auto StepGenerator::publishPrepared(const std::array<std::optional<std::chrono::nanoseconds>, 3>& delays,
                                        const PreparedStart& start) noexcept -> util::Result<>
    {
        auto& work{ *m_splice };
        STEP_LOCK;
        service();
        if (m_status.state != step::State::Running) {
            return fail(std::errc::operation_canceled);
        }
        for (unsigned i{ 0 }; i < 3; ++i) {
            if (delays[i] && (!m_axes[i].finished || m_axes[i].revision != start.revisions[i])) {
                return fail(std::errc::operation_canceled);
            }
        }
        const auto origin{ m_hardware->sample() };
        if (!origin.running || origin.error) {
            return fail(std::errc::io_error);
        }
        for (unsigned i{ 0 }; i < 3; ++i) {
            if (!delays[i]) {
                continue;
            }
            auto& a{ m_axes[i] };
            auto& w{ work.axes[i] };
            for (unsigned half{ 0 }; half < 2; ++half) {
                for (unsigned n{ 0 }; n < w.entries; ++n) {
                    m_hardware->buffers()[i][half][n] =
                      w.edges[half][n] == STEP_PARK ? STEP_PARK : step_add(origin.tick, w.edges[half][n]);
                }
                std::copy_n(w.periods[half].begin(), w.entries / 2, a.periods[half].begin());
            }
            a.generated = w.generated;
            a.completed_pulses = 0;
            a.refill_pending = false;
            a.target = 0;
            a.terminal = w.terminal;
            a.entries = w.entries;
            a.next_rise = step_add(origin.tick, w.next);
            a.first_rise = step_add(origin.tick, w.first);
            a.last_fall = step_add(origin.tick, w.last);
            a.end_tick = end_tick(a.sequence.get(), a.first_rise);
            ++a.revision;
        }
        m_hardware->publish();
        return armPrepared(delays, origin);
    }
    auto StepGenerator::startPrepared(
      const std::array<std::optional<std::chrono::nanoseconds>, 3>& delays) noexcept -> util::Result<>
    {
        if (const auto result{ validateDelays(delays) }; !result) {
            return result;
        }
        preparePending();
        if (m_splicing.test_and_set()) {
            return fail(std::errc::device_or_resource_busy);
        }
        const std::experimental::scope_exit unlock{ [this] { m_splicing.clear(); } };
        PreparedStart start{};
        if (const auto result{ capturePrepared(delays, start) }; !result) {
            return result;
        }
        if (const auto reserved{ reserveScratch(start.capacity) }; !reserved) {
            return reserved;
        }
        auto& work{ *m_splice };
        const std::experimental::scope_exit retire{ [&work] {
            for (auto& axis : work.axes) {
                axis.previous.reset();
            }
        } };
        for (std::size_t i{}; i < delays.size(); ++i) {
            const auto delay{ delays[i] };
            if (!delay) {
                continue;
            }
            if (const auto result{ prepareStartAxis(i, start, *delay) }; !result) {
                return result;
            }
        }
        return publishPrepared(delays, start);
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
            if (!m_hardware->arm(
                  i,
                  m_axes[i].requested == 0
                    ? STEP_PARK
                    : step_add(first, m_axes[i].sequence ? m_axes[i].sequence->initialOffset() : 0),
                  m_axes[i].entries)) {
                for (std::size_t j{}; j < m_axes.size(); ++j) {
                    if (delays[j]) {
                        static_cast<void>(stopAxis(j));
                    }
                }
                return fail(std::errc::timed_out);
            }
            m_axes[i].finished = false;
#if defined(GIGA_MOTION_BENCH)
            giga_motion_bench_startup[i] = {
                origin.tick,
                step_add(first, m_axes[i].sequence ? m_axes[i].sequence->initialOffset() : 0),
                m_hardware->buffers()[i][0][0],
                0,
                0,
                0,
                0,
                0
            };
#endif
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
            ++m_axes[index].revision; // Also cancels an initial preparation outside the lock.
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
        for (std::size_t i{ 0 }; i < m_axes.size(); ++i) {
            if (!m_axes[i].finished) {
                const auto pulses{ observe(i, sample).pulses };
                const auto& axis{ m_axes[i] };
                if (pulses != m_status.pulses[i] && pulses != 0U) {
                    // Read before refilling: a just-completed buffer can still
                    // own the latest pulse until the next rising edge. Retain
                    // its period when subsequent samples see that same count.
                    // A LOW-gap DMA reload rebases bank zero at an arbitrary
                    // pulse ordinal. Bank indexing is relative to that base,
                    // not to the command's original pulse zero.
                    const auto half{ axis.entries / 2U };
                    const auto offset{ pulses > axis.completed_pulses ? pulses - axis.completed_pulses - 1U
                                                                      : std::uint64_t((half * 2U) - 1U) };
                    const auto period{ axis.periods[axis.target ^ ((offset / half) & 1U)][offset % half] };
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
            auto tail{ axis.last_fall };
            if (axis.terminal && axis.end_tick && step_distance(tail, *axis.end_tick) < STEP_HORIZON) {
                tail = *axis.end_tick;
            }
            // Empty timed programs have no last edge to guard.
            if (axis.requested == 0 && axis.end_tick) {
                tail = *axis.end_tick;
            }
            const auto limit{ step_add(tail, axis.terminal ? TERMINAL_GUARD_TICKS : 0U) };
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
    auto StepGenerator::finish(step::State reason, bool send_notification, unsigned site) noexcept -> void
    {
        if (reason == step::State::Underrun || reason == step::State::DmaError) {
            motion_debug_fault_line = site;
        }
        const auto sample{ m_hardware->stop() };
        if (m_status.state != step::State::Running) {
            return;
        }
        updateCounts(sample);
        const auto failure{ sample.error || !m_status.counts_exact ? step::State::DmaError : reason };
        if (failure == step::State::Underrun || failure == step::State::DmaError) {
            // Output inhibition precedes recording; tracing cannot postpone it.
            profile_failure(PROFILE_MOTION_GUARD, GUARD_FAILURE_TAG | unsigned(failure), site, 0);
        }
        for (std::size_t i{ 0 }; i < m_axes.size(); ++i) {
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
            for (auto& axis : m_axes) {
                ++axis.revision;
            }
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
        for (std::size_t i{ 0 }; i < m_axes.size(); ++i) {
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
    bool StepGenerator::completeAxisIfDue(std::size_t i,
                                          const StepSample& sample,
                                          const Observed& observed) noexcept
    {
        auto& axis{ m_axes[i] };
        if (axis.requested && observed.pulses == *axis.requested && !observed.high) {
            if (axis.end_tick && step_distance(sample.tick, *axis.end_tick) > 0 &&
                step_distance(sample.tick, *axis.end_tick) < STEP_HORIZON) {
                if (observed.blocks) {
                    axis.completed_pulses +=
                      static_cast<std::uint64_t>(observed.blocks) * (axis.entries / 2U);
                    axis.target = sample.channels[i].target;
                    m_hardware->acknowledge(i);
                }
                return true;
            }
            m_hardware->finishAxis(i);
            axis.finished = true;
            m_status.axes[i] = step::State::Completed;
            m_status.periods[i] = {};
            return true;
        }
        return false;
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
        for (std::size_t i{ 0 }; i < m_axes.size(); ++i) {
            auto& axis{ m_axes[i] };
            if (axis.finished) {
                continue;
            }
            const auto observed{ observe(i, sample) };
            if (completeAxisIfDue(i, sample, observed)) {
                continue;
            }
            if (observed.blocks == 0U) {
                continue;
            }
            // More than one completed block means the previous deadline was
            // missed. Never try to recover by silently restarting a stream.
            if (observed.blocks != 1U || axis.refill_pending ||
                axis.completed_pulses > std::numeric_limits<std::uint64_t>::max() - (axis.entries / 2U) ||
                (!axis.requested &&
                 axis.generated > std::numeric_limits<std::uint64_t>::max() - (axis.entries / 2U))) {
                finish(step::State::Underrun, true);
                return;
            }
#if defined(GIGA_MOTION_BENCH)
            // The already armed guard uses the last committed falling edge.
            // Capture its remaining time before extending it. Terminal tails
            // need no further generated pulses and are excluded.
            if (!axis.terminal) {
                giga_motion_bench_refill_index[i] = std::uint32_t(axis.generated);
                const auto available{ step_distance(sample.tick, axis.last_fall) };
                giga_motion_bench_refill_ticks = std::min<std::uint32_t>(
                  std::uint32_t(giga_motion_bench_refill_ticks), available < STEP_HORIZON ? available : 0U);
            }
#endif
            axis.completed_pulses += axis.entries / 2U;
            axis.target = sample.channels[i].target;
            m_hardware->acknowledge(i);
            if (m_refillNotification) {
                axis.refill_pending = true;
                axis.refill_tick = sample.tick;
                axis.refill_budget = step_distance(sample.tick, axis.last_fall);
                m_refillNotification();
                continue;
            }
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
