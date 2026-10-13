#pragma once
#include "hal/devices/itf/IDisplay.hpp"
#include "hal/drivers/itf/IRgbOutput.hpp"
#include "hal/drivers/itf/IDigitalOutput.hpp"
#include <memory>
namespace hal::device {
    class SmartwinDisplay final : public IDisplay {
    public:
        SmartwinDisplay(std::shared_ptr<IRgbOutput> output, std::shared_ptr<IDigitalOutput> standby,
                        std::shared_ptr<IDigitalOutput> reset);
        ~SmartwinDisplay() override;
        auto initialize() -> util::Result<> override;
        auto framebuffer(unsigned slot) -> std::span<std::uint16_t> override { return m_output->framebuffer(slot); }
        auto present(unsigned slot) -> util::Result<> override { return m_output->present(slot); }
        auto fill(unsigned slot, std::uint16_t color) -> util::Result<> override { return m_output->fill(slot, color); }
        auto backlight(unsigned percent) -> util::Result<> override { return m_output->backlight(percent); }
        auto underruns() const noexcept -> std::uint32_t override { return m_output->underruns(); }
        auto width() const noexcept -> unsigned override { return m_output->width(); }
        auto height() const noexcept -> unsigned override { return m_output->height(); }
    private:
        std::shared_ptr<IRgbOutput> m_output;
        std::shared_ptr<IDigitalOutput> m_standby, m_reset;
        bool m_initialized{};
    };
}
