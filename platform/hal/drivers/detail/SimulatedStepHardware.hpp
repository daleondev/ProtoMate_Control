#pragma once

#include "hal/drivers/detail/StepHardware.hpp"

#include <algorithm>
#include <functional>
#include <vector>

namespace hal::detail
{
    // Deterministic model of the SAME double-buffer/compare/guard interface.
    // Used by Linux and by tests which deliberately defer IRQs or DMA writes.
    class SimulatedStepHardware : public StepHardware
    {
      public:
        struct Edge
        {
            std::uint64_t tick;
            std::size_t axis;
            bool high;
        };
        StepBuffers memory{};
        StepSample registers{};
        std::array<std::uint32_t, 3> entries{};
        std::array<bool, 3> active{}, high{}, hold_dma{}, pending_dma{};
        std::vector<Edge> edges;
        std::function<void()> interrupt;
        bool interrupts_enabled{ true }, trace_edges{ true };
        std::uint64_t elapsed{};
        std::uint32_t deadline{};

        auto buffers() noexcept -> StepBuffers& override { return memory; }
        auto reset() noexcept -> bool override
        {
            registers = {};
            active = high = pending_dma = {};
            entries = {};
            elapsed = 0U;
            edges.clear();
            return true;
        }
        auto arm(std::size_t axis, std::uint32_t first, std::uint32_t count) noexcept -> void override
        {
            entries[axis] = count;
            registers.channels[axis] = { .compare = first, .remaining = count };
            active[axis] = true;
        }
        auto start(std::uint32_t tick) noexcept -> void override
        {
            deadline = tick;
            registers.running = true;
        }
        auto guard(std::uint32_t tick) noexcept -> void override { deadline = tick; }
        auto sample() noexcept -> StepSample override
        {
            for (std::size_t i = 0; i < registers.channels.size(); ++i) {
                registers.channels[i].tick = registers.tick;
                registers.channels[i].high = high[i];
            }
            return registers;
        }
        auto acknowledge(std::size_t axis) noexcept -> void override
        {
            registers.channels[axis].transfer_complete = false;
        }
        auto finishAxis(std::size_t axis) noexcept -> void override
        {
            active[axis] = false;
            pending_dma[axis] = false;
            registers.channels[axis].compare = step_park;
            acknowledge(axis);
        }
        auto stop() noexcept -> StepSample override
        {
            registers.running = false;
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
        auto publish() noexcept -> void override {}
        auto transfer(std::size_t axis) noexcept -> bool
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
        auto advance(std::uint64_t ticks) noexcept -> void
        {
            while (ticks != 0U && registers.running) {
                auto distance = [this](std::uint32_t target) -> std::uint64_t {
                    const auto value{ stepDistance(registers.tick, target) };
                    return value == 0U ? step_park : value;
                };
                auto next{ distance(deadline) };
                std::array<std::uint64_t, 3> matches;
                for (std::size_t i = 0; i < 3U; ++i) {
                    matches[i] = registers.channels[i].compare == step_park
                                   ? std::uint64_t{ step_park } - registers.tick
                                   : distance(registers.channels[i].compare);
                    next = std::min(next, matches[i]);
                }
                if (next > ticks) {
                    elapsed += ticks;
                    registers.tick = stepAdd(registers.tick, static_cast<std::uint32_t>(ticks));
                    return;
                }
                const bool guard_match{ next == distance(deadline) };
                ticks -= next;
                elapsed += next;
                registers.tick = stepAdd(registers.tick, static_cast<std::uint32_t>(next));
                bool irq{};
                for (std::size_t i = 0; i < 3U; ++i) {
                    if (matches[i] != next) {
                        continue;
                    }
                    auto& channel{ registers.channels[i] };
                    if (channel.compare != step_park) {
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
    };
}
