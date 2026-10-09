#pragma once

#include "hal/drivers/util/StepHardware.hpp"

namespace hal::util
{
    class StepGenerator;
}
namespace hal::stm32
{
    class StepHardware final : public util::StepHardware
    {
      public:
        StepHardware() = default;
        StepHardware(const StepHardware&) = delete;
        StepHardware& operator=(const StepHardware&) = delete;
        StepHardware(StepHardware&&) = delete;
        StepHardware& operator=(StepHardware&&) = delete;
        ~StepHardware() override;
        auto shutdown() noexcept -> void override;
        auto buffers() noexcept -> util::StepBuffers& override;
        auto reset() noexcept -> bool override;
        auto arm(std::size_t axis, std::uint32_t first, std::uint32_t entries) noexcept -> bool override;
        auto start(std::uint32_t deadline) noexcept -> void override;
        auto guard(std::uint32_t tick) noexcept -> void override;
        auto publish() noexcept -> void override;
        auto completionWatch(bool enabled) noexcept -> void override;
        auto sample() noexcept -> util::StepSample override;
        auto acknowledge(std::size_t axis) noexcept -> void override;
        auto stopAxis(std::size_t axis) noexcept -> util::StepSample override;
        auto finishAxis(std::size_t axis) noexcept -> void override;
        auto stop() noexcept -> util::StepSample override;
    };

    // Registration is protected by PRIMASK, before interrupts are armed.
    auto registerStepGenerator(util::StepGenerator* generator) noexcept -> void;
}
