#pragma once

#include "hal/drivers/util/StepHardware.hpp"

#include <functional>
#include <vector>

namespace hal::util
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
        bool completion_watch{};
        std::uint64_t next_poll{};

        auto buffers() noexcept -> StepBuffers& override;
        auto reset() noexcept -> bool override;
        auto arm(std::size_t axis, std::uint32_t first, std::uint32_t count) noexcept -> bool override;
        auto start(std::uint32_t tick) noexcept -> void override;
        auto guard(std::uint32_t tick) noexcept -> void override;
        auto completionWatch(bool enabled) noexcept -> void override;
        auto sample() noexcept -> StepSample override;
        auto acknowledge(std::size_t axis) noexcept -> void override;
        auto finishAxis(std::size_t axis) noexcept -> void override;
        auto stopAxis(std::size_t axis) noexcept -> StepSample override;
        auto stop() noexcept -> StepSample override;
        auto publish() noexcept -> void override;
        auto transfer(std::size_t axis) noexcept -> bool;
        auto advance(std::uint64_t ticks) noexcept -> void;
    };
}
