#include "ui/opcua/util/WifiProfile.hpp"
#include <gtest/gtest.h>
#include <cstring>

using ui::opcua::util::WifiProfile;

TEST(WifiProfile, VersionedRecordAndAllTruncationOrCorruptionBoundaries)
{
    WifiProfile source{};
    std::strcpy(source.credentials.ssid.data(), "test-network");
    std::strcpy(source.credentials.password.data(), "test-only-password");
    source.generation = 0x102030405060708ULL;
    const auto record{ source.encode() };
    WifiProfile decoded{};
    ASSERT_TRUE(WifiProfile::decode(record, decoded));
    EXPECT_EQ(decoded.generation, source.generation);
    EXPECT_EQ(decoded.credentials.ssid, source.credentials.ssid);
    EXPECT_EQ(decoded.credentials.password, source.credentials.password);
    for (std::size_t i{}; i < record.size(); ++i) {
        EXPECT_FALSE(WifiProfile::decode(std::span{ record }.first(i), decoded));
        auto damaged{ record }; damaged[i] ^= 1;
        EXPECT_FALSE(WifiProfile::decode(damaged, decoded));
    }
}

TEST(WifiProfile, InvalidCredentialsOrUncommittedGenerationAreRejected)
{
    WifiProfile source{};
    std::strcpy(source.credentials.ssid.data(), "test-network");
    std::strcpy(source.credentials.password.data(), "password");
    WifiProfile decoded{};
    EXPECT_FALSE(WifiProfile::decode(source.encode(), decoded));
    source.generation = 1;
    ASSERT_TRUE(WifiProfile::decode(source.encode(), decoded));
    source.credentials.password[2] = '\n';
    EXPECT_FALSE(WifiProfile::decode(source.encode(), decoded));
    source.credentials.password.fill('x');
    EXPECT_FALSE(WifiProfile::decode(source.encode(), decoded));
}
