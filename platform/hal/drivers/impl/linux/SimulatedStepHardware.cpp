#include "SimulatedStepHardware.hpp"

#include <algorithm>

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
        const auto lead{ stepDistance(registers.tick, first) };
        if (!registers.running || lead < step_min_phase || lead > step_horizon) {
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
        if (enabled && !completion_watch)
            next_poll = elapsed + 10'000U;
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
        registers.channels[axis].compare = step_park;
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

    auto SimulatedStepHardware::advance(std::uint64_t ticks) noexcept -> void
    {
        while (ticks != 0U && registers.running) {
            auto distance = [this](std::uint32_t target) -> std::uint64_t {
                const auto value{ stepDistance(registers.tick, target) };
                return value == 0U ? step_park : value;
            };
            auto next{ deadline == step_park ? std::uint64_t{ step_park } - registers.tick
                                             : distance(deadline) };
            std::array<std::uint64_t, 3> matches;
            for (std::size_t i = 0; i < 3U; ++i) {
                matches[i] = registers.channels[i].compare == step_park
                               ? std::uint64_t{ step_park } - registers.tick
                               : distance(registers.channels[i].compare);
                next = std::min(next, matches[i]);
            }
            if (completion_watch)
                next = std::min(next, next_poll - elapsed);
            if (next > ticks) {
                elapsed += ticks;
                registers.tick = stepAdd(registers.tick, static_cast<std::uint32_t>(ticks));
                return;
            }
            const bool guard_match{ deadline != step_park && next == distance(deadline) };
            ticks -= next;
            elapsed += next;
            registers.tick = stepAdd(registers.tick, static_cast<std::uint32_t>(next));
            bool irq{};
            if (completion_watch && elapsed == next_poll) {
                next_poll += 10'000U;
                irq = true;
            }
            for (std::size_t i = 0; i < 3U; ++i) {
                if (matches[i] != next) {
                    continue;
                }
                auto& channel{ registers.channels[i] };
                if (active[i] && channel.compare != step_park) {
                    high[i] = !high[i];
                    if (trace_edges) {
                        edges.push_back({ elapsed, i, high[i] });
                    }
                }
                channel.compare_pending = true;
                pending_dma[i] = active[i];
                irq |= transfer(i);
            }
            if (guard_match) {
                registers.running = false;
                irq = true;
            }
            if (irq && interrupts_enabled && interrupt) {
                interrupt();
            }
        }
    }
}
