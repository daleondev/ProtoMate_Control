#include "hal/board/board.hpp"

#include "hal/devices/impl/Button.hpp"
#include "hal/devices/impl/Led.hpp"
#include "hal/drivers/factory/gpio.hpp"

#include <memory>
#include <utility>

namespace hal::board
{
    namespace
    {
        using enum gpio::Port;

        [[nodiscard]] auto make_led(gpio::Pin pin) -> std::shared_ptr<device::ILed>
        {
            auto output{ gpio::createOutput(gpio::OutputConfiguration{
              .pin = pin,
              .initial_level = gpio::Level::Low,
              .type = gpio::OutputType::PushPull,
              .pull = gpio::Pull::None,
              .speed = gpio::Speed::Low,
            }) };
            if (output == nullptr) {
                return {};
            }
            return std::make_shared<device::Led>(std::move(output), gpio::Level::High);
        }

        [[nodiscard]] auto make_user_button() -> std::shared_ptr<device::IButton>
        {
            auto input{ gpio::createInput(gpio::InputConfiguration{
              .pin = { .port = C, .number = 13U },
              .pull = gpio::Pull::Down,
              .edge = gpio::Edge::Both,
            }) };
            if (input == nullptr) {
                return {};
            }
            return std::make_shared<device::Button>(std::move(input), gpio::Level::High);
        }
    }

    auto createLed(LedId id) -> std::shared_ptr<device::ILed>
    {
        switch (id) {
            using enum LedId;
            case Green: {
                static const auto led{ make_led({ .port = B, .number = 0U }) };
                return led;
            }
            case Yellow: {
                static const auto led{ make_led({ .port = E, .number = 1U }) };
                return led;
            }
            case Red: {
                static const auto led{ make_led({ .port = B, .number = 14U }) };
                return led;
            }
        }
        return {};
    }

    auto createButton(ButtonId id) -> std::shared_ptr<device::IButton>
    {
        switch (id) {
            using enum ButtonId;
            case User: {
                static const auto button{ make_user_button() };
                return button;
            }
        }
        return {};
    }
}
