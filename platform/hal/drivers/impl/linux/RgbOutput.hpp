#pragma once
#include "hal/drivers/itf/IRgbOutput.hpp"
#include <mutex>
namespace hal
{
    class RgbOutput final : public IRgbOutput
    {
      public:
        RgbOutput() = default;
        RgbOutput(const RgbOutput&) = delete;
        auto operator=(const RgbOutput&) -> RgbOutput& = delete;
        RgbOutput(RgbOutput&&) = delete;
        auto operator=(RgbOutput&&) -> RgbOutput& = delete;

        ~RgbOutput() override;
        auto initialize() -> util::Result<> override;
        auto framebuffer(unsigned slot) -> std::span<std::uint16_t> override;
        auto present(unsigned slot) -> util::Result<> override;
        auto requestPresent(unsigned slot) -> util::Result<> override;
        auto state() const noexcept -> display::State override;
        void setCallback(display::Callback callback) noexcept override;
        void shutdown() noexcept override;
        void service() noexcept override;
        auto servicePeriod() const noexcept -> std::chrono::milliseconds override
        {
            return std::chrono::milliseconds{ 1 };
        }
        auto fill(unsigned slot, std::uint16_t color) -> util::Result<> override;
        auto backlight(unsigned percent) -> util::Result<> override;
        auto underruns() const noexcept -> std::uint32_t override { return 0; }
        auto width() const noexcept -> unsigned override { return WIDTH; }
        auto height() const noexcept -> unsigned override { return HEIGHT; }

      private:
        static constexpr unsigned WIDTH{ 1024 }, HEIGHT{ 600 }, PERCENT{ 100 };
        bool m_initialized{};
        std::mutex m_callbackMutex;
        display::Callback m_callback;
        display::State m_observed;
    };
}
