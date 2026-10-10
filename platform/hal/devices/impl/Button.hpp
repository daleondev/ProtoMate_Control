#pragma once

#include "hal/devices/itf/IButton.hpp"
#include "hal/drivers/itf/IDigitalInput.hpp"

#include <memory>

namespace hal::device
{
    class Button final : public IButton
    {
      public:
        Button(const Button&) = delete;
        auto operator=(const Button&) -> Button& = delete;
        Button(Button&&) = delete;
        auto operator=(Button&&) -> Button& = delete;

        Button(std::shared_ptr<IDigitalInput> input, gpio::Level active_level);
        ~Button() override;

        [[nodiscard]] auto state() const noexcept -> State override;
        auto setStateChangedCallback(StateChangedCallback callback) noexcept -> void override;

      private:
        [[nodiscard]] auto stateFromLevel(gpio::Level level) const noexcept -> State;

        std::shared_ptr<IDigitalInput> m_input;
        gpio::Level m_activeLevel;
    };
}
