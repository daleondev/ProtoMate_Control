#include "hal/core/core.hpp"
#include "hal/core/Catalog.hpp"
#include "hal/board/board.hpp"

#include <gtest/gtest.h>

namespace
{
    using Core = hal::core::Id;
    constexpr auto M4 = hal::core::devices_for(Core::M4);
    constexpr auto M7 = hal::core::devices_for(Core::M7);
    constexpr auto HOST = hal::core::devices_for(Core::Host);

    // Exercise both hardware catalogs without selecting another executing core
    // or touching real hardware: the board underneath these tests is virtual.
    TEST(HalCoreCatalog, M4HasOnlyItsAssignedRoles)
    {
        EXPECT_EQ(M4.status_led, nullptr);
        ASSERT_NE(M4.runtime_timer, nullptr);
        EXPECT_EQ(M4.runtime_timer(), hal::board::create_timer(hal::timer::Peripheral::Tim5));
        EXPECT_EQ(M4.error_led, nullptr);
        EXPECT_EQ(M4.user_button, nullptr);
    }

    TEST(HalCoreCatalog, M7HasItsAssignedLedsAndButton)
    {
        ASSERT_NE(M7.status_led, nullptr);
        EXPECT_EQ(M7.status_led(), hal::board::create_led(hal::board::LedId::Green));
        ASSERT_NE(M7.error_led, nullptr);
        EXPECT_EQ(M7.error_led(), hal::board::create_led(hal::board::LedId::Red));
        ASSERT_NE(M7.user_button, nullptr);
        EXPECT_EQ(M7.user_button(), hal::board::create_button(hal::board::ButtonId::User));
        ASSERT_NE(M7.runtime_timer, nullptr);
        ASSERT_TRUE(M7.runtime_timer());
        EXPECT_EQ(M7.runtime_timer(), hal::board::create_timer(hal::timer::Peripheral::Tim12));
        EXPECT_NE(M7.runtime_timer(), M4.runtime_timer());
    }

    TEST(HalCoreCatalog, SharedCapabilitiesUseTheSameBoardFactories)
    {
        EXPECT_EQ(M4.rtc, hal::board::create_rtc);
        EXPECT_EQ(M7.rtc, hal::board::create_rtc);
        EXPECT_EQ(M4.rng, hal::board::create_rng);
        EXPECT_EQ(M7.rng, hal::board::create_rng);
        EXPECT_EQ(M4.intercore_channel, hal::board::create_intercore_channel);
        EXPECT_EQ(M7.intercore_channel, hal::board::create_intercore_channel);
    }

    TEST(HalCoreCatalog, MotorPeripheralsBelongToM4)
    {
        EXPECT_EQ(M4.stepper_generator, hal::board::create_stepper_generator);
        EXPECT_EQ(M4.stepper_step_output, hal::board::create_stepper_step_output);
        EXPECT_EQ(M4.stepper_direction_output, hal::board::create_stepper_direction_output);
        EXPECT_EQ(M4.steppers_enable_output, hal::board::create_steppers_enable_output);
        EXPECT_EQ(M4.reference_limit_switch, hal::board::create_reference_limit_switch);
        EXPECT_EQ(M4.stepper_driver_bus, hal::board::create_stepper_driver_bus);
        EXPECT_EQ(M4.stepper_drivers, hal::board::create_stepper_drivers);
        EXPECT_EQ(M4.motor_feedback, hal::board::create_motor_feedback);
        EXPECT_EQ(M7.stepper_generator, nullptr);
        EXPECT_EQ(M7.stepper_step_output, nullptr);
        EXPECT_EQ(M7.stepper_direction_output, nullptr);
        EXPECT_EQ(M7.steppers_enable_output, nullptr);
        EXPECT_EQ(M7.reference_limit_switch, nullptr);
        EXPECT_EQ(M7.stepper_driver_bus, nullptr);
        EXPECT_EQ(M7.stepper_drivers, nullptr);
        EXPECT_EQ(M7.motor_feedback, nullptr);
    }

    TEST(HalCore, ConstructsStoppedMotorPeripherals)
    {
        using namespace pnm::units::literals;
        auto enable = hal::core::create_steppers_enable_output();
        ASSERT_TRUE(enable);
        EXPECT_EQ(enable->read(), hal::gpio::Level::High);
        auto generator = hal::core::create_stepper_generator();
        ASSERT_TRUE(generator);
        auto bus = hal::core::create_stepper_driver_bus();
        ASSERT_TRUE(bus);
        const auto drivers = hal::core::create_stepper_drivers({16, 16, 16}, enable, bus);
        for (const auto id : {hal::core::MotorId::Motor1, hal::core::MotorId::Motor2, hal::core::MotorId::Motor3}) {
            auto output = hal::core::create_stepper_step_output(generator, id);
            ASSERT_TRUE(output);
            EXPECT_NE(output->status().state, hal::step::State::Running);
            auto direction = hal::core::create_stepper_direction_output(id);
            ASSERT_TRUE(direction);
            EXPECT_EQ(direction->read(), hal::gpio::Level::Low);
            EXPECT_TRUE(hal::core::create_reference_limit_switch(id));
            auto feedback = hal::core::create_motor_feedback(id, {1.8_deg, 16});
            ASSERT_TRUE(feedback);
            EXPECT_EQ(feedback->source(), id == hal::core::MotorId::Motor1
                ? hal::device::IMotorFeedback::Source::ShaftEncoder
                : hal::device::IMotorFeedback::Source::DriverIndex);
            EXPECT_TRUE(drivers[static_cast<std::size_t>(id)]);
        }
        EXPECT_EQ(enable->read(), hal::gpio::Level::High);
        EXPECT_FALSE(hal::core::create_stepper_direction_output(static_cast<hal::core::MotorId>(255)));
        EXPECT_FALSE(hal::core::create_reference_limit_switch(static_cast<hal::core::MotorId>(255)));
    }

    TEST(HalCore, HostSelectsItsOwnCatalog)
    {
        EXPECT_EQ(hal::core::id(), Core::Host);
        EXPECT_EQ(HOST.status_led, nullptr);
        EXPECT_EQ(HOST.intercore_channel, nullptr);
        EXPECT_FALSE(hal::core::create_status_led());
        EXPECT_FALSE(hal::core::create_intercore_channel());
        ASSERT_TRUE(hal::core::create_runtime_timer());
        EXPECT_EQ(hal::core::create_runtime_timer(), HOST.runtime_timer());
        EXPECT_EQ(hal::core::create_user_button(), HOST.user_button());
        EXPECT_EQ(hal::core::create_error_led(), HOST.error_led());
        EXPECT_EQ(hal::core::create_rtc(), HOST.rtc());
        EXPECT_EQ(hal::core::create_rng(), HOST.rng());
    }

    TEST(HalBoard, AccessIsIndependentOfCoreAssignments)
    {
        // Host has no assigned status LED, but the board still exposes all LEDs.
        for (const auto id : {hal::board::LedId::Green, hal::board::LedId::Green, hal::board::LedId::Red}) {
            EXPECT_TRUE(hal::board::create_led(id));
        }
        EXPECT_TRUE(hal::board::create_button(hal::board::ButtonId::User));
        EXPECT_FALSE(hal::board::create_led(static_cast<hal::board::LedId>(255)));
        EXPECT_FALSE(hal::board::create_button(static_cast<hal::board::ButtonId>(255)));
        EXPECT_FALSE(hal::board::create_timer(static_cast<hal::timer::Peripheral>(255)));
    }
}
