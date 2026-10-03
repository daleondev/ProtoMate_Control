#pragma once

#include "hal/drivers/detail/PwmTiming.hpp"
#include "hal/drivers/detail/TimerLease.hpp"
#include "hal/drivers/factory/pwm.hpp"
#include "hal/drivers/itf/IDigitalOutput.hpp"
#include "hal/hal.hpp"

#include <memory>

namespace hal
{
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
        auto selectPinMode(bool alternate) noexcept -> void;
        detail::TimerLease m_lease;
        std::shared_ptr<IDigitalOutput> m_pin;
        TIM_HandleTypeDef& m_handle;
        GPIO_TypeDef* m_port;
        std::uint32_t m_pinMask;
        std::uint32_t m_channel;
        std::uint32_t m_alternate;
        std::uint32_t m_inputHz;
        detail::PwmTiming m_timing{};
        bool m_running{};
    };
}
