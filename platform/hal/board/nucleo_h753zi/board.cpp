#include "hal/board/board.hpp"

#include "hal/devices/impl/Button.hpp"
#include "hal/devices/impl/Led.hpp"
#include "hal/drivers/factory/encoder.hpp"
#include "hal/drivers/factory/gpio.hpp"
#include "hal/drivers/factory/pwm.hpp"

#include <memory>
#include <utility>

namespace hal::board
{
    namespace
    {
        using enum gpio::Port;

        [[nodiscard]] auto make_stepper_output(gpio::Pin pin, gpio::Level initial_level)
          -> std::shared_ptr<IDigitalOutput>
        {
            return gpio::createOutput({
              .pin = pin,
              .initial_level = initial_level,
              .type = gpio::OutputType::PushPull,
              .pull = gpio::Pull::None,
              .speed = gpio::Speed::Low,
            });
        }

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

    auto createStepperStepOutput(MotorId id) -> std::shared_ptr<IPwmOutput>
    {
        using enum gpio::Port;
        switch (id) {
            case MotorId::M1:
                return pwm::create({ .timer = 1U, .channel = 1U, .pin = { E, 9U } });
            case MotorId::M2:
                return pwm::create({ .timer = 4U, .channel = 3U, .pin = { D, 14U } });
            case MotorId::M3:
                return pwm::create({ .timer = 8U, .channel = 1U, .pin = { C, 6U } });
        }
        return {};
    }

    auto createStepperDirectionOutput(MotorId id) -> std::shared_ptr<IDigitalOutput>
    {
        using enum gpio::Port;
        switch (id) {
            case MotorId::M1:
                return make_stepper_output({ E, 11U }, gpio::Level::Low);
            case MotorId::M2:
                return make_stepper_output({ D, 15U }, gpio::Level::Low);
            case MotorId::M3:
                return make_stepper_output({ C, 7U }, gpio::Level::Low);
        }
        return {};
    }

    auto createSteppersEnableOutput() -> std::shared_ptr<IDigitalOutput>
    {
        return make_stepper_output({ gpio::Port::F, 3U }, gpio::Level::High);
    }

    auto createEncoder(MotorId id) -> std::shared_ptr<IQuadratureEncoder>
    {
        if (id != MotorId::M1) {
            return {};
        }
        return encoder::create({ .timer = 3U, .a = { gpio::Port::B, 4U }, .b = { gpio::Port::B, 5U } });
    }

    auto createEncoderIndex(MotorId id) -> std::shared_ptr<IDigitalInput>
    {
        if (id != MotorId::M1) {
            return {};
        }
        return gpio::createInput({ .pin = { gpio::Port::B, 6U }, .edge = gpio::Edge::Rising });
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
