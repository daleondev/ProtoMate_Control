#include "hal/board/board.hpp"
#include "hal/devices/impl/Button.hpp"
#include "hal/devices/impl/Led.hpp"
#include "hal/drivers/factory/gpio.hpp"
#include "hal/drivers/impl/linux/Gpio.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>

namespace
{
    using enum hal::gpio::Port;
}

TEST(HalGpioDrivers, OutputSupportsAtomicStateOperationsAndExclusiveOwnership)
{
    constexpr hal::gpio::Pin pin{ .port = A, .number = 0U };
    auto output{ hal::gpio::create_output(hal::gpio::OutputConfiguration{
      .pin = pin, .initial_level = hal::gpio::Level::High, .alternate_function = std::nullopt }) };
    ASSERT_NE(output, nullptr);
    EXPECT_EQ(output->read(), hal::gpio::Level::High);

    output->write(hal::gpio::Level::Low);
    EXPECT_EQ(output->read(), hal::gpio::Level::Low);
    output->toggle();
    EXPECT_EQ(output->read(), hal::gpio::Level::High);

    EXPECT_EQ(hal::gpio::create_input({ .pin = pin, .alternate_function = std::nullopt }), nullptr);
    output.reset();
    EXPECT_NE(hal::gpio::create_input({ .pin = pin, .alternate_function = std::nullopt }), nullptr);

    const auto invalid_port{ static_cast<hal::gpio::Port>(UINT8_MAX) };
    const auto invalid_port_output{
        hal::gpio::create_output({ .pin = { .port = invalid_port, .number = 0U }, .alternate_function = std::nullopt })
    };
    const auto invalid_pin_output{ hal::gpio::create_output({ .pin = { .port = A, .number = 16U }, .alternate_function = std::nullopt }) };
    EXPECT_EQ(invalid_port_output, nullptr);
    EXPECT_EQ(invalid_pin_output, nullptr);
}

TEST(HalGpioDrivers, PeripheralInputRetainsExclusivePinOwnership)
{
    constexpr hal::gpio::Pin pin{ .port = B, .number = 4U };
    EXPECT_EQ(hal::gpio::create_input({ .pin = pin, .alternate_function = 16U }), nullptr);
    EXPECT_EQ(
      hal::gpio::create_input({ .pin = pin, .edge = hal::gpio::Edge::Rising, .alternate_function = 2U }),
      nullptr);

    auto input{ hal::gpio::create_input({ .pin = pin, .alternate_function = 2U }) };
    ASSERT_NE(input, nullptr);
    EXPECT_EQ(hal::gpio::create_output({ .pin = pin, .alternate_function = std::nullopt }), nullptr);
    EXPECT_EQ(hal::gpio::create_input({ .pin = pin, .alternate_function = std::nullopt }), nullptr);

    input.reset();
    EXPECT_NE(hal::gpio::create_output({ .pin = pin, .alternate_function = std::nullopt }), nullptr);
}

TEST(HalGpioDrivers, SimulationAccessDrivesExistingInputWithoutClaimingAnotherPin)
{
    constexpr hal::gpio::Pin pin{ .port = A, .number = 3U };
    EXPECT_EQ(hal::gpio::simulated_input(pin), nullptr);
    EXPECT_EQ(hal::gpio::simulated_input({ .port = A, .number = 16U }), nullptr);
    EXPECT_EQ(hal::gpio::simulated_input({ .port = static_cast<hal::gpio::Port>(UINT8_MAX), .number = 0U }),
              nullptr);

    auto input{ hal::gpio::create_input({ .pin = pin, .pull = hal::gpio::Pull::Up,
                                       .edge = hal::gpio::Edge::Both, .alternate_function = std::nullopt }) };
    ASSERT_NE(input, nullptr);
    auto simulated{ hal::gpio::simulated_input(pin) };
    ASSERT_EQ(simulated, input);
    EXPECT_EQ(hal::gpio::create_input({ .pin = pin, .alternate_function = std::nullopt }), nullptr);
    EXPECT_EQ(hal::gpio::create_output({ .pin = pin, .alternate_function = std::nullopt }), nullptr);

    unsigned callbacks{};
    input->setEdgeCallback([&](hal::gpio::Level level) noexcept {
        ++callbacks;
        // Callbacks run outside the registry lock and see the updated level.
        const auto observed{ hal::gpio::simulated_input(pin) };
        ASSERT_EQ(observed, input);
        EXPECT_EQ(observed->read(), level);
    });
    simulated->setSimulatedLevel(hal::gpio::Level::Low);
    EXPECT_EQ(input->read(), hal::gpio::Level::Low);
    EXPECT_EQ(callbacks, 1U);
    simulated->setSimulatedLevel(hal::gpio::Level::High);
    EXPECT_EQ(input->read(), hal::gpio::Level::High);
    EXPECT_EQ(callbacks, 2U);

    input.reset();
    simulated.reset();
    EXPECT_EQ(hal::gpio::simulated_input(pin), nullptr);
    auto output{ hal::gpio::create_output({ .pin = pin, .alternate_function = std::nullopt }) };
    ASSERT_NE(output, nullptr);
    EXPECT_EQ(hal::gpio::simulated_input(pin), nullptr);
}

TEST(HalGpioDrivers, PeripheralOutputRetainsExclusivePinOwnership)
{
    constexpr hal::gpio::Pin pin{ .port = C, .number = 6U };
    EXPECT_EQ(hal::gpio::create_output({ .pin = pin, .alternate_function = 16U }), nullptr);

    auto output{ hal::gpio::create_output({ .pin = pin, .alternate_function = 3U }) };
    ASSERT_NE(output, nullptr);
    EXPECT_EQ(output->read(), hal::gpio::Level::Low);
    EXPECT_EQ(hal::gpio::create_input({ .pin = pin, .alternate_function = std::nullopt }), nullptr);
    EXPECT_EQ(hal::gpio::create_output({ .pin = pin, .alternate_function = std::nullopt }), nullptr);

    output.reset();
    EXPECT_NE(hal::gpio::create_input({ .pin = pin, .alternate_function = std::nullopt }), nullptr);
}

TEST(HalGpioDevices, LedAppliesActivePolarity)
{
    auto active_low_output{ hal::gpio::create_output(hal::gpio::OutputConfiguration{
      .pin = { .port = A, .number = 1U }, .initial_level = hal::gpio::Level::High, .alternate_function = std::nullopt }) };
    ASSERT_NE(active_low_output, nullptr);

    hal::device::Led led{ active_low_output, hal::gpio::Level::Low };
    EXPECT_FALSE(led.isOn());
    led.turnOn();
    EXPECT_TRUE(led.isOn());
    EXPECT_EQ(active_low_output->read(), hal::gpio::Level::Low);
    led.toggle();
    EXPECT_FALSE(led.isOn());
}

TEST(HalGpioDevices, ButtonTranslatesBothEdgesUsingActivePolarity)
{
    auto input{ std::make_shared<hal::GpioInput>(hal::gpio::InputConfiguration{
      .pin = { .port = A, .number = 2U }, .pull = hal::gpio::Pull::Down, .edge = hal::gpio::Edge::Both, .alternate_function = std::nullopt }) };
    hal::device::Button button{ input, hal::gpio::Level::High };
    EXPECT_FALSE(button.isPressed());

    hal::device::IButton::State observed{ hal::device::IButton::State::Released };
    unsigned int callback_count{};
    button.setStateChangedCallback([&](hal::device::IButton::State state) noexcept {
        observed = state;
        ++callback_count;
    });

    input->setSimulatedLevel(hal::gpio::Level::High);
    EXPECT_TRUE(button.isPressed());
    EXPECT_EQ(observed, hal::device::IButton::State::Pressed);
    EXPECT_EQ(callback_count, 1U);

    input->setSimulatedLevel(hal::gpio::Level::High);
    EXPECT_EQ(callback_count, 1U);

    input->setSimulatedLevel(hal::gpio::Level::Low);
    EXPECT_FALSE(button.isPressed());
    EXPECT_EQ(observed, hal::device::IButton::State::Released);
    EXPECT_EQ(callback_count, 2U);
}

TEST(HalBoardDevices, NucleoFactoriesAreStableAndInitiallyInactive)
{
    const auto green{ hal::board::create_led(hal::board::LedId::Green) };
    const auto green_again{ hal::board::create_led(hal::board::LedId::Green) };
    ASSERT_NE(green, nullptr);
    EXPECT_EQ(green, green_again);
    EXPECT_FALSE(green->isOn());

    const auto yellow{ hal::board::create_led(hal::board::LedId::Yellow) };
    const auto yellow_again{ hal::board::create_led(hal::board::LedId::Yellow) };
    ASSERT_NE(yellow, nullptr);
    EXPECT_EQ(yellow, yellow_again);
    EXPECT_FALSE(yellow->isOn());

    const auto button{ hal::board::create_button(hal::board::ButtonId::User) };
    const auto button_again{ hal::board::create_button(hal::board::ButtonId::User) };
    ASSERT_NE(button, nullptr);
    EXPECT_EQ(button, button_again);
    EXPECT_FALSE(button->isPressed());
}
