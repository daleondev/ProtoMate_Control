#pragma once

#include "hal/drivers/detail/SimulatedStepHardware.hpp"
#include "hal/drivers/detail/StepGenerator.hpp"

#include <chrono>

namespace hal::detail
{
    // Linux is a logical simulation. Queries advance virtual hardware and
    // service its DMA interrupts; it makes no real-time GPIO guarantee.
    class LinuxStepHardware final : public SimulatedStepHardware
    {
      public:
        LinuxStepHardware() { trace_edges = false; }
        auto start(std::uint32_t tick) noexcept -> void override
        {
            m_last = std::chrono::steady_clock::now();
            SimulatedStepHardware::start(tick);
        }
        auto sample() noexcept -> StepSample override
        {
            update();
            return SimulatedStepHardware::sample();
        }
        auto stop() noexcept -> StepSample override
        {
            update();
            return SimulatedStepHardware::stop();
        }

      private:
        auto update() noexcept -> void
        {
            if (m_advancing || !registers.running) {
                return;
            }
            m_advancing = true;
            const auto now{ std::chrono::steady_clock::now() };
            const auto ticks{ std::chrono::duration_cast<std::chrono::nanoseconds>(now - m_last).count() /
                              step_tick_ns };
            if (ticks > 0) {
                m_last += std::chrono::nanoseconds{ ticks * step_tick_ns };
                advance(static_cast<std::uint64_t>(ticks));
            }
            m_advancing = false;
        }
        std::chrono::steady_clock::time_point m_last{};
        bool m_advancing{};
    };
}
