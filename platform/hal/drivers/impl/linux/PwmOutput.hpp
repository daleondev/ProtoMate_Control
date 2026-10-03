#pragma once

#include "hal/linux/Mutex.hpp"

#include "hal/drivers/detail/PulseTrain.hpp"
#include "hal/drivers/detail/PwmTiming.hpp"
#include "hal/drivers/detail/TimerLease.hpp"
#include "hal/drivers/factory/pwm.hpp"
#include "hal/drivers/itf/IDigitalOutput.hpp"

#include <memory>
#include <mutex>

namespace hal
{
    // Configuration/state simulation; does not toggle Linux GPIOs or run a
    // scheduler thread pretending to reproduce electrical pulse timing.
    class PwmOutput final : public IPwmOutput
    {
      public:
        PwmOutput(pwm::Configuration configuration,
                  std::uint8_t alternate,
                  detail::TimerLease lease,
                  std::shared_ptr<IDigitalOutput> pin);
        ~PwmOutput() override;
        [[nodiscard]] auto configure(Timing timing) noexcept -> util::Result<> override;
        [[nodiscard]] auto timing() const noexcept -> Timing override;
        [[nodiscard]] auto start() noexcept -> util::Result<> override;
        [[nodiscard]] auto startPulses(std::optional<PulseCount> count = std::nullopt) noexcept
          -> util::Result<> override;
        [[nodiscard]] auto pulseCount() const noexcept -> PulseCount override;
        [[nodiscard]] auto setPulseCallback(PulseCallback callback) noexcept -> util::Result<> override;
        [[nodiscard]] auto stop() noexcept -> util::Result<> override;
        [[nodiscard]] auto isRunning() const noexcept -> bool override;

        // Deterministic electrical-event simulation; no wall-clock scheduler.
        auto beginSimulatedPulse() noexcept -> void;
        auto finishSimulatedPulse() noexcept -> void;
        auto advanceSimulatedPulses(PulseCount count) noexcept -> void;

      private:
        detail::TimerLease m_lease; // Released after the pin.
        std::shared_ptr<IDigitalOutput> m_pin;
        // Serialize event delivery/registration, while permitting callback queries.
        mutable linux::Mutex m_mutex{ true };
        detail::PwmTiming m_timing{};
        detail::PulseTrain m_train;
        PulseCallback m_callback;
        bool m_counted{};
        bool m_running{};
    };
}
