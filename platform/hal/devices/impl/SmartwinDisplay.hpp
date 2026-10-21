#pragma once
#include "hal/devices/itf/IDisplay.hpp"
#include "hal/drivers/itf/IDigitalOutput.hpp"
#include "hal/drivers/itf/IDigitalInput.hpp"
#include "hal/drivers/itf/IRgbOutput.hpp"
#include <memory>
namespace hal::device
{
    class SmartwinDisplay final : public IDisplay
    {
      public:
        SmartwinDisplay(std::shared_ptr<IRgbOutput> output,
                        std::shared_ptr<IDigitalOutput> standby,
                        std::shared_ptr<IDigitalOutput> reset,
                        std::shared_ptr<IDigitalInput> powerReady = {});
        ~SmartwinDisplay() override;
        auto initialize() -> util::Result<> override;
        auto framebuffer(unsigned slot) -> std::span<std::uint16_t> override
        {
            return m_output->framebuffer(slot);
        }
        auto present(unsigned slot) -> util::Result<> override { return m_output->present(slot); }
        auto requestPresent(unsigned slot) -> util::Result<> override
        {
            return m_output->requestPresent(slot);
        }
        auto state() const noexcept -> hal::display::State override { return m_output->state(); }
        void setCallback(hal::display::Callback callback) noexcept override
        {
            m_output->setCallback(callback);
        }
        void shutdown() noexcept override;
        void service() noexcept override;
        auto servicePeriod() const noexcept -> std::chrono::milliseconds override;
        auto fill(unsigned slot, std::uint16_t color) -> util::Result<> override
        {
            return m_output->fill(slot, color);
        }
        auto backlight(unsigned percent) -> util::Result<> override;
        auto underruns() const noexcept -> std::uint32_t override { return m_output->underruns(); }
        auto width() const noexcept -> unsigned override { return m_output->width(); }
        auto height() const noexcept -> unsigned override { return m_output->height(); }

      private:
        std::shared_ptr<IRgbOutput> m_output;
        std::shared_ptr<IDigitalOutput> m_standby, m_reset;
        std::shared_ptr<IDigitalInput> m_powerReady;
        enum class Phase { Unpowered, ResetHold, ResetReleased, Starting, AwaitFrame, Active };
        Phase m_phase{Phase::Unpowered};
        std::chrono::steady_clock::time_point m_deadline{};
        unsigned m_brightness{};
        std::uint32_t m_frameSync{};
        bool m_initialized{};
    };
}
