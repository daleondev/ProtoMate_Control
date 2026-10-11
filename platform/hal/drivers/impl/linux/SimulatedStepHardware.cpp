#include "SimulatedStepHardware.hpp"

#include <algorithm>

namespace
{
    inline constexpr auto COMPLETION_POLL_TICKS{ 10'000U };
}

namespace hal::util
{
    auto SimulatedStepHardware::buffers() noexcept -> StepBuffers& { return memory; }

    auto SimulatedStepHardware::reset() noexcept -> bool
    {
        registers = {};
        active = high = pending_dma = {};
        entries = {};
        completion_watch = false;
        elapsed = 0U;
        edges.clear();
        return true;
    }

    auto SimulatedStepHardware::arm(std::size_t axis, std::uint32_t first, std::uint32_t count) noexcept
      -> bool
    {
        const auto lead{ step_distance(registers.tick, first) };
        if (!registers.running || lead < STEP_MIN_PHASE || lead > STEP_HORIZON) {
            return false;
        }
        high[axis] = pending_dma[axis] = false;
        entries[axis] = count;
        registers.channels[axis] = { .compare = first, .remaining = count };
        active[axis] = true;
        return true;
    }

    auto SimulatedStepHardware::start(std::uint32_t tick) noexcept -> void
    {
        deadline = tick;
        registers.running = true;
    }

    auto SimulatedStepHardware::guard(std::uint32_t tick) noexcept -> void { deadline = tick; }

    auto SimulatedStepHardware::completionWatch(bool enabled) noexcept -> void
    {
        if (enabled && !completion_watch) {
            next_poll = elapsed + COMPLETION_POLL_TICKS;
        }
        completion_watch = enabled;
    }

    auto SimulatedStepHardware::sample() noexcept -> StepSample
    {
        for (std::size_t i = 0; i < registers.channels.size(); ++i) {
            registers.channels[i].tick = registers.tick;
            registers.channels[i].high = high[i];
        }
        return registers;
    }

    auto SimulatedStepHardware::acknowledge(std::size_t axis) noexcept -> void
    {
        registers.channels[axis].transfer_complete = false;
    }

    auto SimulatedStepHardware::finishAxis(std::size_t axis) noexcept -> void
    {
        active[axis] = false;
        pending_dma[axis] = false;
        registers.channels[axis].compare = STEP_PARK;
        acknowledge(axis);
    }

    auto SimulatedStepHardware::stopAxis(std::size_t axis) noexcept -> StepSample
    {
        const auto result{ sample() };
        finishAxis(axis);
        if (high[axis] && trace_edges) {
            edges.push_back({ elapsed, axis, false });
        }
        high[axis] = false;
        return result;
    }

    auto SimulatedStepHardware::stop() noexcept -> StepSample
    {
        registers.running = false;
        completion_watch = false;
        const auto result{ SimulatedStepHardware::sample() };
        active = {};
        for (std::size_t i = 0; i < high.size(); ++i) {
            if (high[i] && trace_edges) {
                edges.push_back({ elapsed, i, false });
            }
            high[i] = false;
        }
        return result;
    }

    auto SimulatedStepHardware::publish() noexcept -> void {}

    auto SimulatedStepHardware::transfer(std::size_t axis) noexcept -> bool
    {
        if (!active[axis] || !pending_dma[axis] || hold_dma[axis]) {
            return false;
        }
        auto& channel{ registers.channels[axis] };
        pending_dma[axis] = false;
        channel.compare = memory[axis][channel.target][entries[axis] - channel.remaining];
        --channel.remaining;
        if (channel.remaining == 0U) {
            channel.remaining = entries[axis];
            channel.target ^= 1U;
            channel.transfer_complete = true;
            return true;
        }
        return channel.remaining == entries[axis] / 2U;
    }

    auto SimulatedStepHardware::compareEvent(std::size_t axis) noexcept -> bool
    {
        auto& channel{ registers.channels[axis] };
        if (active[axis] && channel.compare != STEP_PARK) {
            high[axis] = !high[axis];
            if (trace_edges) {
                edges.push_back({ elapsed, axis, high[axis] });
            }
        }
        channel.compare_pending = true;
        pending_dma[axis] = active[axis];
        return transfer(axis);
    }

    auto SimulatedStepHardware::dispatchEvents(const std::array<std::uint64_t, 3>& matches,
                                               std::uint64_t next,
                                               bool guard_match) noexcept -> void
    {
        bool irq{};
        if (completion_watch && elapsed == next_poll) {
            next_poll += COMPLETION_POLL_TICKS;
            irq = true;
        }
        for (std::size_t i = 0; i < 3U; ++i) {
            if (matches[i] != next) {
                continue;
            }
            irq |= compareEvent(i);
        }
        if (guard_match) {
            registers.running = false;
            irq = true;
        }
        if (irq && interrupts_enabled && interrupt) {
            interrupt();
        }
    }

    auto SimulatedStepHardware::advance(std::uint64_t ticks) noexcept -> void
    {
        while (ticks != 0U && registers.running) {
            auto distance = [this](std::uint32_t target) -> std::uint64_t {
                const auto value{ step_distance(registers.tick, target) };
                return value == 0U ? STEP_PARK : value;
            };
            auto next{ deadline == STEP_PARK ? std::uint64_t{ STEP_PARK } - registers.tick
                                             : distance(deadline) };
            std::array<std::uint64_t, 3> matches{};
            for (std::size_t i = 0; i < 3U; ++i) {
                matches[i] = registers.channels[i].compare == STEP_PARK
                               ? std::uint64_t{ STEP_PARK } - registers.tick
                               : distance(registers.channels[i].compare);
                next = std::min(next, matches[i]);
            }
            if (completion_watch) {
                next = std::min(next, next_poll - elapsed);
            }
            if (next > ticks) {
                elapsed += ticks;
                registers.tick = step_add(registers.tick, static_cast<std::uint32_t>(ticks));
                return;
            }
            const bool guard_match{ deadline != STEP_PARK && next == distance(deadline) };
            ticks -= next;
            elapsed += next;
            registers.tick = step_add(registers.tick, static_cast<std::uint32_t>(next));
            dispatchEvents(matches, next, guard_match);
        }
    }
}
