// Host-only device composition for regression tests. These virtual resources
// do not specify or reserve any pins on the physical controller.
#include "hal/board/board.hpp"
#include "hal/drivers/factory/intercore.hpp"
#include "hal/drivers/factory/rtc.hpp"
#include "hal/drivers/factory/rng.hpp"

#include "hal/devices/factory/tmc2209.hpp"
#include "hal/devices/impl/Button.hpp"
#include "hal/devices/impl/IndexFeedback.hpp"
#include "hal/devices/impl/Led.hpp"
#include "hal/devices/impl/QuadratureEncoderFeedback.hpp"
#include "hal/drivers/factory/encoder.hpp"
#include "hal/drivers/factory/gpio.hpp"
#include "hal/drivers/factory/spi.hpp"
#include "hal/drivers/factory/step.hpp"
#include "hal/drivers/factory/timer.hpp"

#include "hal/devices/impl/Dm542tDriver.hpp"
#include "hal/devices/impl/Tmc2209Driver.hpp"
#include <bit>
#include <memory>
#include <stdexcept>
#include <utility>

namespace hal::board
{
    auto create_rtc(util::Core owner) -> std::shared_ptr<IRtc>
    { return owner == util::executing_core() ? rtc::create() : nullptr; }
    auto create_rng(util::Core owner) -> std::shared_ptr<IRng>
    { return owner == util::executing_core() ? rng::create() : nullptr; }

    auto create_intercore_channel(util::Core owner) -> std::shared_ptr<IInterCoreChannel>
    {
        return intercore::create(owner);
    }

    auto create_status_led(util::Core) -> std::shared_ptr<device::ILed>
    {
        // Host composition has no physical core status LED.
        return {};
    }

    auto create_runtime_timer(util::Core owner) -> std::shared_ptr<ITimer>
    {
        if (owner != util::Core::Host) {
            return {};
        }
        return timer::create({.peripheral = timer::Peripheral::Tim5});
    }
    namespace
    {
        constexpr auto MAX_MICROSTEPS{ 256U };
        constexpr auto ENCODER_COUNTS_PER_TURN{ 1600U };
        constexpr std::array<std::uint16_t, 2> NOMINAL_CURRENT_MA{ 650, 550 };
        constexpr std::array<std::uint16_t, 2> RATED_CURRENT_MA{ 700, 590 };
        using enum gpio::Port;
        constexpr gpio::Pin USER_BUTTON_PIN{ .port = C, .number = 13U };
        constexpr gpio::Pin ETHERCAT_SELECT_PIN{ .port = F, .number = 6U };
        constexpr gpio::Pin M1_DIRECTION_PIN{ .port = E, .number = 12U };
        constexpr gpio::Pin M2_DIRECTION_PIN{ .port = E, .number = 13U };
        constexpr gpio::Pin M3_DIRECTION_PIN{ .port = E, .number = 14U };
        constexpr gpio::Pin STEPPERS_ENABLE_PIN{ .port = E, .number = 15U };
        constexpr gpio::Pin ENCODER_A_PIN{ .port = B, .number = 4U };
        constexpr gpio::Pin ENCODER_B_PIN{ .port = B, .number = 5U };
        constexpr gpio::Pin ENCODER_INDEX_PIN{ .port = B, .number = 6U };
        constexpr gpio::Pin M1_REFERENCE_PIN{ .port = E, .number = 7U };
        constexpr gpio::Pin M2_REFERENCE_PIN{ .port = E, .number = 8U };
        constexpr gpio::Pin M3_REFERENCE_PIN{ .port = E, .number = 10U };
        constexpr gpio::Pin RED_LED_PIN{ .port = B, .number = 14U };

        [[nodiscard]] auto make_stepper_output(gpio::Pin pin, gpio::Level initial_level)
          -> std::shared_ptr<IDigitalOutput>
        {
            return gpio::create_output({
              .pin = pin,
              .initial_level = initial_level,
              .type = gpio::OutputType::PushPull,
              .pull = gpio::Pull::None,
              .speed = gpio::Speed::Low,
            });
        }

        [[nodiscard]] auto make_led(gpio::Pin pin) -> std::shared_ptr<device::ILed>
        {
            auto output{ gpio::create_output(gpio::OutputConfiguration{
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
            auto input{ gpio::create_input(gpio::InputConfiguration{
              .pin = USER_BUTTON_PIN,
              .pull = gpio::Pull::Down,
              .edge = gpio::Edge::Both,
            }) };
            if (input == nullptr) {
                return {};
            }
            return std::make_shared<device::Button>(std::move(input), gpio::Level::High);
        }
    }

    auto create_stepper_generator() -> std::shared_ptr<IStepGenerator> { return step::create(); }

    auto create_ethercat_spi() -> std::shared_ptr<ISpi>
    {
        return spi::create({ .peripheral = spi::Peripheral::Spi5 });
    }

    auto create_ethercat_chip_select() -> std::shared_ptr<IDigitalOutput>
    {
        return gpio::create_output(
          { .pin = ETHERCAT_SELECT_PIN, .initial_level = gpio::Level::High, .pull = gpio::Pull::Up });
    }

    auto create_stepper_driver_bus() -> std::shared_ptr<IUart>
    {
        // PD5 TX is coupled to PD6 RX through a resistor. Let late replies finish
        // before flushing the line and beginning another request.
        return device::tmc2209::create_transport({
          .peripheral = uart::Peripheral::Usart2,
          .transport = { .local_echo = true, .settle_time = std::chrono::milliseconds{ 1 } },
        });
    }

    auto create_stepper_diagnostic(MotorId id) -> std::shared_ptr<IDigitalInput>
    {
        if (id == MotorId::Motor1) {
            return gpio::create_input(
              { .pin = { .port = F, .number = 2 }, .pull = gpio::Pull::Up, .edge = gpio::Edge::Rising });
        }
        if (id != MotorId::Motor2 && id != MotorId::Motor3) {
            return {};
        }
        return gpio::create_input(
          { .pin = { .port = D, .number = static_cast<std::uint8_t>(id == MotorId::Motor2 ? 4 : 3) },
            .pull = gpio::Pull::Down,
            .edge = gpio::Edge::Rising });
    }

    auto create_stepper_index(MotorId id) -> std::shared_ptr<IDigitalInput>
    {
        if (id != MotorId::Motor2 && id != MotorId::Motor3) {
            return {};
        }
        return gpio::create_input(
          { .pin = { .port = D, .number = static_cast<std::uint8_t>(id == MotorId::Motor2 ? 0 : 1) },
            .pull = gpio::Pull::Down,
            .edge = gpio::Edge::Both });
    }

    auto create_motor_feedback(MotorId id, const MotorFeedbackConfig& config)
      -> std::shared_ptr<device::IMotorFeedback>
    {
        if (!config.full_step_angle.isFinite() || config.full_step_angle.get() <= 0.0 || !config.microsteps) {
            throw std::invalid_argument("invalid motor feedback configuration");
        }
        if (id == MotorId::Motor1) {
            auto encoder{ create_encoder(id) };
            auto index{ create_encoder_index(id) };
            if (!encoder || !index) {
                return {};
            }
            return std::make_shared<device::QuadratureEncoderFeedback>(
              std::move(encoder), encoder_counts_per_revolution(id), std::move(index));
        }
        if (id == MotorId::Motor2 || id == MotorId::Motor3) {
            auto index{ create_stepper_index(id) };
            if (!index) {
                return {};
            }
            return std::make_shared<device::IndexFeedback>(
              std::move(index), config.full_step_angle, config.microsteps);
        }
        return {};
    }

    auto create_stepper_drivers(const std::array<std::size_t, 3>& microsteps,
                                const std::shared_ptr<IDigitalOutput>& enable,
                                std::shared_ptr<IUart> uart)
      -> std::array<std::shared_ptr<device::IStepperDriver>, 3>
    {
        if (!microsteps[0] || !std::has_single_bit(microsteps[1]) || microsteps[1] > MAX_MICROSTEPS ||
            !std::has_single_bit(microsteps[2]) || microsteps[2] > MAX_MICROSTEPS) {
            throw std::invalid_argument("invalid driver microsteps");
        }
        if (!enable) {
            return {};
        }
        if (!uart) {
            uart = create_stepper_driver_bus();
        }
        if (!uart) {
            return {};
        }
        auto bus{ std::make_shared<device::Tmc2209Bus>(std::move(uart), std::vector<std::uint8_t>{ 0, 1 }) };
        std::array<std::shared_ptr<device::IStepperDriver>, 3> drivers;
        auto alarm{ create_stepper_diagnostic(MotorId::Motor1) };
        if (!alarm) {
            return {};
        }
        drivers[0] = std::make_shared<device::Dm542tDriver>(std::move(alarm), enable);
        for (std::size_t i{ 1 }; i < drivers.size(); ++i) {
            auto diag{ create_stepper_diagnostic(static_cast<MotorId>(i)) };
            if (!diag) {
                return {};
            }
            device::IConfigurableStepperDriver::Configuration config{
                .run_milliamps = NOMINAL_CURRENT_MA[i - 1],
                .hold_milliamps = NOMINAL_CURRENT_MA[i - 1],
                .microsteps = static_cast<std::uint16_t>(microsteps[i]),
                .mode = device::IConfigurableStepperDriver::Mode::StealthChop
            };
            // Motor phase-current limits; Z must retain its full holding current.
            device::IConfigurableStepperDriver::Limits limits{ .maximum_run_milliamps =
                                                                 RATED_CURRENT_MA[i - 1],
                                                               .hold_equals_run = i == 2 };
            drivers[i] = std::make_shared<device::Tmc2209Driver>(
              bus, static_cast<std::uint8_t>(i - 1), std::move(diag), enable, config, limits);
        }
        return drivers;
    }

    auto create_stepper_step_output(const std::shared_ptr<IStepGenerator>& generator, MotorId id)
      -> std::shared_ptr<IStepOutput>
    {
        if (!generator) {
            return {};
        }
        switch (id) {
            case MotorId::Motor1:
                return generator->output(step::Axis::M1);
            case MotorId::Motor2:
                return generator->output(step::Axis::M2);
            case MotorId::Motor3:
                return generator->output(step::Axis::M3);
        }
        return {};
    }

    auto create_stepper_direction_output(MotorId id) -> std::shared_ptr<IDigitalOutput>
    {
        using enum gpio::Port;
        switch (id) {
            case MotorId::Motor1:
                return make_stepper_output(M1_DIRECTION_PIN, gpio::Level::Low);
            case MotorId::Motor2:
                return make_stepper_output(M2_DIRECTION_PIN, gpio::Level::Low);
            case MotorId::Motor3:
                return make_stepper_output(M3_DIRECTION_PIN, gpio::Level::Low);
        }
        return {};
    }

    auto create_steppers_enable_output() -> std::shared_ptr<IDigitalOutput>
    {
        return make_stepper_output(STEPPERS_ENABLE_PIN, gpio::Level::High);
    }

    auto encoder_counts_per_revolution(MotorId id) noexcept -> std::uint32_t
    {
        // PKP245D23A2-R2FL: 400 P/R, all four quadrature edges counted.
        return id == MotorId::Motor1 ? ENCODER_COUNTS_PER_TURN : 0U;
    }

    auto create_encoder(MotorId id) -> std::shared_ptr<IQuadratureEncoder>
    {
        if (id != MotorId::Motor1) {
            return {};
        }
        return encoder::create(
          { .timer = hal::timer::Peripheral::Tim3, .a = ENCODER_A_PIN, .b = ENCODER_B_PIN });
    }

    auto create_encoder_index(MotorId id) -> std::shared_ptr<IDigitalInput>
    {
        if (id != MotorId::Motor1) {
            return {};
        }
        return gpio::create_input({ .pin = ENCODER_INDEX_PIN, .edge = gpio::Edge::Rising });
    }

    auto create_reference_limit_switch(MotorId id) -> std::shared_ptr<IDigitalInput>
    {
        gpio::Pin pin{};
        switch (id) {
            case MotorId::Motor1:
                pin = M1_REFERENCE_PIN;
                break;
            case MotorId::Motor2:
                pin = M2_REFERENCE_PIN;
                break;
            case MotorId::Motor3:
                pin = M3_REFERENCE_PIN;
                break;
            default:
                return {};
        }
        return gpio::create_input({ .pin = pin, .pull = gpio::Pull::Up, .edge = gpio::Edge::Both });
    }

    auto create_led(LedId id) -> std::shared_ptr<device::ILed>
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
                static const auto led{ make_led(RED_LED_PIN) };
                return led;
            }
        }
        return {};
    }

    auto create_button(ButtonId id) -> std::shared_ptr<device::IButton>
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
