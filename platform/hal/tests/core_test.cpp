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
        ASSERT_NE(M4.status_led, nullptr);
        EXPECT_EQ(M4.status_led(), hal::board::create_led(hal::board::LedId::Green));
        ASSERT_NE(M4.runtime_timer, nullptr);
        EXPECT_EQ(M4.runtime_timer(), hal::board::create_timer(hal::timer::Peripheral::Tim5));
        EXPECT_EQ(M4.error_led, nullptr);
        EXPECT_EQ(M4.user_button, nullptr);
    }

    TEST(HalCoreCatalog, M7HasItsAssignedLedsAndButton)
    {
        ASSERT_NE(M7.status_led, nullptr);
        EXPECT_EQ(M7.status_led(), hal::board::create_led(hal::board::LedId::Yellow));
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
        for (const auto id : {hal::board::LedId::Green, hal::board::LedId::Yellow, hal::board::LedId::Red}) {
            EXPECT_TRUE(hal::board::create_led(id));
        }
        EXPECT_TRUE(hal::board::create_button(hal::board::ButtonId::User));
        EXPECT_FALSE(hal::board::create_led(static_cast<hal::board::LedId>(255)));
        EXPECT_FALSE(hal::board::create_button(static_cast<hal::board::ButtonId>(255)));
        EXPECT_FALSE(hal::board::create_timer(static_cast<hal::timer::Peripheral>(255)));
    }
}
