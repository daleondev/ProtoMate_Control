#pragma once

#include "hal/drivers/detail/PulseTrain.hpp"
#include "hal/drivers/detail/PwmTiming.hpp"
#include "hal/drivers/detail/TimerLease.hpp"
#include "hal/drivers/factory/pwm.hpp"
#include "hal/drivers/itf/IDigitalOutput.hpp"
#include "hal/hal.hpp"

#include <array>
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
        [[nodiscard]] auto startPulses(std::optional<PulseCount> count = std::nullopt) noexcept
          -> util::Result<> override;
        [[nodiscard]] auto pulseCount() const noexcept -> PulseCount override;
        [[nodiscard]] auto setPulseCallback(PulseCallback callback) noexcept -> util::Result<> override;
        [[nodiscard]] auto stop() noexcept -> util::Result<> override;
        [[nodiscard]] auto isRunning() const noexcept -> bool override;
        static auto dispatchInterrupt(std::uint8_t timer) noexcept -> void;

      private:
        auto selectPinMode(bool alternate) noexcept -> void;
        [[nodiscard]] auto prepare(bool counted) noexcept -> util::Result<>;
        [[nodiscard]] auto stopImpl(bool notify) noexcept -> util::Result<>;
        auto disableInterrupts() noexcept -> void;
        auto handleInterrupt() noexcept -> void;
        detail::TimerLease m_lease;
        std::shared_ptr<IDigitalOutput> m_pin;
        TIM_HandleTypeDef& m_handle;
        GPIO_TypeDef* m_port;
        std::uint32_t m_pinMask;
        std::uint32_t m_channel;
        std::uint32_t m_alternate;
        std::uint32_t m_inputHz;
        std::uint8_t m_timer;
        IRQn_Type m_updateIrq;
        IRQn_Type m_compareIrq;
        std::uint32_t m_compareFlag;
        detail::PwmTiming m_timing{};
        detail::PulseTrain m_train;
        PulseCallback m_callback;
        bool m_counted{};
        bool m_running{};
        inline static std::array<PwmOutput*, 9U> s_instances{};
    };
}
