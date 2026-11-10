#pragma once

#include "hal/drivers/impl/StepGenerator.hpp"
#include "hal/drivers/util/StepHardware.hpp"

#include "simulation/Client.hpp"
#include <chrono>

namespace hal::util
{
    // Register/DMA proxy only. Electrical events execute in the native simulation process.
    class LinuxStepHardware final : public StepHardware
    {
      public:
        LinuxStepHardware(const LinuxStepHardware&) = delete;
        auto operator=(const LinuxStepHardware&) -> LinuxStepHardware& = delete;
        LinuxStepHardware(LinuxStepHardware&&) = delete;
        auto operator=(LinuxStepHardware&&) -> LinuxStepHardware& = delete;

        LinuxStepHardware() = default;
        auto buffers() noexcept -> StepBuffers& override { return m_memory; }
        ~LinuxStepHardware() override;
        static void beginService(StepGenerator& generator);
        void stopService() noexcept override;
        auto reset() noexcept -> bool override;
        auto arm(std::size_t axis, std::uint32_t first, std::uint32_t count) noexcept -> bool override;
        auto guard(std::uint32_t tick) noexcept -> void override;
        auto completionWatch(bool enabled) noexcept -> void override;
        auto acknowledge(std::size_t axis) noexcept -> void override;
        auto finishAxis(std::size_t axis) noexcept -> void override;
        auto stopAxis(std::size_t axis) noexcept -> StepSample override;
        auto publish() noexcept -> void override;
        bool supportsRetarget() const noexcept override { return true; }
        void retarget(std::size_t axis, std::uint32_t tick) noexcept override;
        void spliceWindow(std::optional<std::uint32_t> cut) noexcept override { m_cut = cut; }
        auto start(std::uint32_t tick) noexcept -> void override;
        auto sample() noexcept -> StepSample override;
        auto stop() noexcept -> StepSample override;

      private:
        std::optional<std::uint32_t> m_cut;
        bool m_publicationError{};
        StepBuffers m_memory{};
        StepBuffers m_published{};
        std::array<std::array<bool, 2>, 3> m_valid{};
    };
}
