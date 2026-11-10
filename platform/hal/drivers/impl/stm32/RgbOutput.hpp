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
        RgbOutput(const RgbOutput&) = delete;
        auto operator=(const RgbOutput&) -> RgbOutput& = delete;
        RgbOutput(RgbOutput&&) = delete;
        auto operator=(RgbOutput&&) -> RgbOutput& = delete;

        RgbOutput(LTDC_HandleTypeDef& lcd, TIM_HandleTypeDef& pwm, std::shared_ptr<IDigitalOutput> backlight);
        ~RgbOutput() override;
        auto initialize() -> util::Result<> override;
        auto framebuffer(unsigned slot) -> std::span<std::uint16_t> override;
        auto present(unsigned slot) -> util::Result<> override;
        auto requestPresent(unsigned slot) -> util::Result<> override;
        auto state() const noexcept -> display::State override;
        void setCallback(display::Callback callback) noexcept override;
        void shutdown() noexcept override;
        auto fill(unsigned slot, std::uint16_t color) -> util::Result<> override;
        auto backlight(unsigned percent) -> util::Result<> override;
        auto underruns() const noexcept -> std::uint32_t override { return state().underruns; }
        auto width() const noexcept -> unsigned override { return WIDTH; }
        auto height() const noexcept -> unsigned override { return HEIGHT; }
        void interrupt() noexcept;

      private:
        static constexpr unsigned WIDTH{ 1024 }, HEIGHT{ 600 }, PERCENT{ 100 };
        LTDC_HandleTypeDef& m_lcd;
        TIM_HandleTypeDef& m_pwm;
        std::shared_ptr<IDigitalOutput> m_backlight;
        TX_SEMAPHORE m_completion{};
        display::Callback m_callback;
        display::State m_state;
        bool m_initialized{};
    };
}
