#include "hal/core/core.hpp"
#include "hal/board/board.hpp"
#include "hal/board/nucleo_h755ziq/resources.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace
{
    using Core = hal::core::Id;
    using hal::board::resources::allows;

    TEST(HalCoreOwnership, PinAssignmentsAreExclusiveAndUnassignedPinsAreDenied)
    {
        unsigned m4_count{};
        unsigned m7_count{};
        for (unsigned port{}; port <= static_cast<unsigned>(hal::gpio::Port::K); ++port) {
            for (std::uint8_t number{}; number < 16; ++number) {
                const hal::gpio::Pin pin{static_cast<hal::gpio::Port>(port), number};
                const bool m4 = allows(Core::M4, pin);
                const bool m7 = allows(Core::M7, pin);
                EXPECT_FALSE(m4 && m7);
                EXPECT_FALSE(allows(Core::Host, pin));
                m4_count += m4;
                m7_count += m7;
            }
        }
        EXPECT_EQ(m4_count, 1U);
        EXPECT_EQ(m7_count, 1U);
        EXPECT_TRUE(allows(Core::M4, hal::gpio::Pin{hal::gpio::Port::B, 0}));
        EXPECT_TRUE(allows(Core::M7, hal::gpio::Pin{hal::gpio::Port::E, 1}));
        EXPECT_FALSE(allows(Core::M4, hal::gpio::Pin{hal::gpio::Port::B, 16}));
        EXPECT_FALSE(allows(Core::M7, hal::gpio::Pin{static_cast<hal::gpio::Port>(255), 1}));
    }

    TEST(HalCoreOwnership, TimersAreExclusiveAndUnknownIdsAreDenied)
    {
        for (unsigned value{}; value <= 255; ++value) {
            const auto timer = static_cast<hal::timer::Peripheral>(value);
            EXPECT_EQ(allows(Core::M4, timer), timer == hal::timer::Peripheral::Tim5);
            EXPECT_EQ(allows(Core::M7, timer), timer == hal::timer::Peripheral::Tim12);
            EXPECT_FALSE(allows(Core::Host, timer));
            EXPECT_FALSE(allows(static_cast<Core>(255), timer));
        }
    }

    TEST(HalCore, HostDoesNotImpersonateEitherHardwareCore)
    {
        EXPECT_EQ(hal::core::id(), Core::Host);
        EXPECT_FALSE(hal::core::create_status_led());
        const auto timer = hal::core::create_runtime_timer();
        ASSERT_TRUE(timer);
        EXPECT_EQ(timer, hal::board::create_runtime_timer(Core::Host));
        EXPECT_FALSE(hal::board::create_runtime_timer(Core::M4));
        EXPECT_FALSE(hal::board::create_runtime_timer(Core::M7));
    }
}
