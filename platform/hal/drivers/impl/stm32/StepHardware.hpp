#pragma once

#include "hal/drivers/detail/StepHardware.hpp"
#include <memory>

namespace hal::detail
{
    class StepGenerator;
}
namespace hal::stm32
{
    class StepHardware final : public detail::StepHardware
    {
      public:
        StepHardware() = default;
        StepHardware(const StepHardware&) = delete;
        StepHardware& operator=(const StepHardware&) = delete;
        StepHardware(StepHardware&&) = delete;
        StepHardware& operator=(StepHardware&&) = delete;
        ~StepHardware() override;
        auto shutdown() noexcept -> void override;
        auto buffers() noexcept -> detail::StepBuffers& override;
        auto reset() noexcept -> bool override;
        auto arm(std::size_t axis, std::uint32_t first, std::uint32_t entries) noexcept -> bool override;
        auto start(std::uint32_t deadline) noexcept -> void override;
        auto guard(std::uint32_t tick) noexcept -> void override;
        auto publish() noexcept -> void override;
        auto completionWatch(bool enabled) noexcept -> void override;
        auto sample() noexcept -> detail::StepSample override;
        auto acknowledge(std::size_t axis) noexcept -> void override;
        auto stopAxis(std::size_t axis) noexcept -> detail::StepSample override;
        auto finishAxis(std::size_t axis) noexcept -> void override;
        auto stop() noexcept -> detail::StepSample override;
    };

    [[nodiscard]] auto makeStepHardware() -> std::unique_ptr<detail::StepHardware>;
    // Registration is protected by PRIMASK, before interrupts are armed.
    auto registerStepGenerator(detail::StepGenerator* generator) noexcept -> void;
}
