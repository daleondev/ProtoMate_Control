#pragma once

#include "hal/drivers/impl/StepGenerator.hpp"
#include "hal/drivers/impl/linux/SimulatedStepHardware.hpp"

#include <chrono>

namespace hal::util
{
    // Linux is a logical simulation, serviced by a background ThreadX worker
    // and synchronous queries. It makes no real-time GPIO guarantee.
    class LinuxStepHardware final : public SimulatedStepHardware
    {
      public:
        LinuxStepHardware();
        ~LinuxStepHardware() override;
        void beginService(StepGenerator& generator);
        void stopService() noexcept override;
        auto start(std::uint32_t tick) noexcept -> void override;
        auto sample() noexcept -> StepSample override;
        auto stop() noexcept -> StepSample override;

      private:
        struct Service;
        std::unique_ptr<Service> m_service;
        auto update() noexcept -> void;
        std::chrono::steady_clock::time_point m_last{};
        bool m_advancing{};
    };
}
