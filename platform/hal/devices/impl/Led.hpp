#pragma once

#include "hal/devices/itf/ILed.hpp"
#include "hal/drivers/itf/IDigitalOutput.hpp"

#include <memory>

namespace hal::device
{
    class Led final : public ILed
    {
      public:
        Led(const Led&) = delete;
        auto operator=(const Led&) -> Led& = delete;
        Led(Led&&) = delete;
        auto operator=(Led&&) -> Led& = delete;

        Led(std::shared_ptr<IDigitalOutput> output, gpio::Level active_level);

        ~Led() override = default;

        auto turnOn() noexcept -> void override;
        auto turnOff() noexcept -> void override;
        auto toggle() noexcept -> void override;
        [[nodiscard]] auto isOn() const noexcept -> bool override;

      private:
        std::shared_ptr<IDigitalOutput> m_output;
        gpio::Level m_activeLevel;
    };
}
