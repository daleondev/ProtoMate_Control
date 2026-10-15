#pragma once
#include "hal/drivers/itf/IDigitalOutput.hpp"
#include "hal/drivers/itf/IRgbOutput.hpp"
#include "stm32h7xx_hal.h"
#include <memory>
#include <tx_api.h>
namespace hal
{
    class RgbOutput final : public IRgbOutput
    {
      public:
        RgbOutput(LTDC_HandleTypeDef& lcd, TIM_HandleTypeDef& pwm, std::shared_ptr<IDigitalOutput> backlight);
        ~RgbOutput() override;
        auto initialize() -> util::Result<> override;
        auto framebuffer(unsigned slot) -> std::span<std::uint16_t> override;
        auto present(unsigned slot) -> util::Result<> override;
        auto requestPresent(unsigned slot) -> util::Result<> override;
        auto state() const noexcept -> display::State override;
        void setCallback(display::Callback callback) noexcept override;
        void shutdown() noexcept override;
        auto fill(unsigned slot, std::uint16_t rgb565) -> util::Result<> override;
        auto backlight(unsigned percent) -> util::Result<> override;
        auto underruns() const noexcept -> std::uint32_t override { return state().underruns; }
        auto width() const noexcept -> unsigned override { return 1024; }
        auto height() const noexcept -> unsigned override { return 600; }
        void interrupt() noexcept;

      private:
        LTDC_HandleTypeDef& m_lcd;
        TIM_HandleTypeDef& m_pwm;
        std::shared_ptr<IDigitalOutput> m_backlight;
        TX_SEMAPHORE m_completion{};
        display::Callback m_callback;
        display::State m_state;
        bool m_initialized{};
    };
}
