#include "hal/board/board.hpp"

#include "hal/devices/impl/Button.hpp"
#include "hal/devices/impl/Led.hpp"
#include "hal/drivers/factory/encoder.hpp"
#include "hal/drivers/factory/gpio.hpp"
#include "hal/drivers/factory/step.hpp"

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

    auto createStepperGenerator() -> std::shared_ptr<IStepGenerator> { return step::create(); }

    auto createStepperStepOutput(const std::shared_ptr<IStepGenerator>& generator, MotorId id)
      -> std::shared_ptr<IStepOutput>
    {
        if (!generator) {
            return {};
        }
        switch (id) {
            case MotorId::Motor1:
                return generator->output(step::Axis::_1);
            case MotorId::Motor2:
                return generator->output(step::Axis::_2);
            case MotorId::Motor3:
                return generator->output(step::Axis::_3);
        }
        return {};
    }

    auto createStepperDirectionOutput(MotorId id) -> std::shared_ptr<IDigitalOutput>
    {
        using enum gpio::Port;
        switch (id) {
            case MotorId::Motor1:
                return make_stepper_output({ E, 12U }, gpio::Level::Low);
            case MotorId::Motor2:
                return make_stepper_output({ E, 13U }, gpio::Level::Low);
            case MotorId::Motor3:
                return make_stepper_output({ E, 14U }, gpio::Level::Low);
        }
        return {};
    }

    auto createSteppersEnableOutput() -> std::shared_ptr<IDigitalOutput>
    {
        return make_stepper_output({ gpio::Port::E, 15U }, gpio::Level::High);
    }

    auto encoderCountsPerRevolution(MotorId id) noexcept -> std::uint32_t
    {
        // PKP245D23A2-R2FL: 400 P/R, all four quadrature edges counted.
        return id == MotorId::Motor1 ? 1600U : 0U;
    }

    auto createEncoder(MotorId id) -> std::shared_ptr<IQuadratureEncoder>
    {
        if (id != MotorId::Motor1) {
            return {};
        }
        return encoder::create({ .timer = 3U, .a = { gpio::Port::B, 4U }, .b = { gpio::Port::B, 5U } });
    }

    auto createEncoderIndex(MotorId id) -> std::shared_ptr<IDigitalInput>
    {
        if (id != MotorId::Motor1) {
            return {};
        }
        return gpio::createInput({ .pin = { gpio::Port::B, 6U }, .edge = gpio::Edge::Rising });
    }

    auto createReferenceLimitSwitch(MotorId id) -> std::shared_ptr<IDigitalInput>
    {
        gpio::Pin pin{};
        switch (id) {
            case MotorId::Motor1:
                pin = { E, 7U };
                break;
            case MotorId::Motor2:
                pin = { E, 8U };
                break;
            case MotorId::Motor3:
                pin = { E, 10U };
                break;
            default:
                return {};
        }
        return gpio::createInput({ .pin = pin, .pull = gpio::Pull::Up, .edge = gpio::Edge::Both });
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
