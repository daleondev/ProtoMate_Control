#include "hal/drivers/factory/rng.hpp"

#include <gtest/gtest.h>

TEST(HalRngDrivers, FactoryIsStableAndGeneratesValues)
{
    const auto rng{ hal::rng::create() };
    const auto rng_again{ hal::rng::create() };

    ASSERT_NE(rng, nullptr);
    EXPECT_EQ(rng, rng_again);
    EXPECT_TRUE(rng->generate().has_value());
    EXPECT_TRUE(rng->generate().has_value());
}
