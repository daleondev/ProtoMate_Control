#pragma once

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
        [[nodiscard]] auto stop() noexcept -> util::Result<> override;
        [[nodiscard]] auto isRunning() const noexcept -> bool override;

      private:
        detail::TimerLease m_lease; // Released after the pin.
        std::shared_ptr<IDigitalOutput> m_pin;
        mutable std::mutex m_mutex;
        detail::PwmTiming m_timing{};
        bool m_running{};
    };
}
