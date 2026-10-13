#pragma once
#include "hal/drivers/itf/IRgbOutput.hpp"
#include "hal/drivers/itf/IDigitalOutput.hpp"
#include "stm32h7xx_hal.h"
#include <memory>
namespace hal {
    class RgbOutput final : public IRgbOutput {
    public:
        RgbOutput(LTDC_HandleTypeDef& lcd, DMA2D_HandleTypeDef& dma, TIM_HandleTypeDef& pwm, std::shared_ptr<IDigitalOutput> backlight);
        ~RgbOutput() override;
        auto initialize() -> util::Result<> override;
        auto framebuffer(unsigned slot) -> std::span<std::uint16_t> override;
        auto present(unsigned slot) -> util::Result<> override;
        auto fill(unsigned slot, std::uint16_t rgb565) -> util::Result<> override;
        auto backlight(unsigned percent) -> util::Result<> override;
        auto underruns() const noexcept -> std::uint32_t override;
        auto width() const noexcept -> unsigned override { return 1024; }
        auto height() const noexcept -> unsigned override { return 600; }
    private:
        LTDC_HandleTypeDef& m_lcd;
        DMA2D_HandleTypeDef& m_dma;
        TIM_HandleTypeDef& m_pwm;
        std::shared_ptr<IDigitalOutput> m_backlight;
        bool m_initialized{};
        unsigned m_active{};
    };
}
