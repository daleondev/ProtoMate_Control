#include "hal/devices/impl/St1633i.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <array>

namespace {
class ResetPin final : public hal::IDigitalOutput {
public:
    hal::gpio::Level value{hal::gpio::Level::Low};
    auto read() const noexcept -> hal::gpio::Level override { return value; }
    void write(hal::gpio::Level next) noexcept override { value=next; }
    void toggle() noexcept override {}
};
class TouchBus final : public hal::II2c {
public:
    bool present{true}, failRead{};
    std::uint8_t status{};
    std::array<std::uint8_t,20> points{};
    auto probe(std::uint8_t address, std::chrono::milliseconds timeout) -> hal::util::Result<> override
    {
        EXPECT_EQ(address,0x55); EXPECT_LE(timeout.count(),20);
        if (!present) { return std::unexpected(std::make_error_code(std::errc::timed_out)); }
        return {};
    }
    auto readRegister(std::uint8_t address, std::uint8_t reg, std::span<std::uint8_t> bytes,
                      std::chrono::milliseconds timeout) -> hal::util::Result<> override
    {
        EXPECT_EQ(address,0x55); EXPECT_LE(timeout.count(),20);
        if (failRead) { return std::unexpected(std::make_error_code(std::errc::io_error)); }
        if (reg==0) { EXPECT_EQ(bytes.size(),2u); bytes[0]=0x31;bytes[1]=status; }
        else { EXPECT_EQ(reg,0x12); EXPECT_EQ(bytes.size(),points.size()); std::ranges::copy(points,bytes.begin()); }
        return {};
    }
};
}
TEST(TouchDisplay, NoReadUntilSuccessfulBoundedProbe)
{
    auto bus=std::make_shared<TouchBus>(); auto reset=std::make_shared<ResetPin>();
    hal::device::St1633i touch(bus,0x55,reset,{});
    EXPECT_EQ(touch.read().error(),std::make_error_code(std::errc::not_connected));
    bus->present=false;
    EXPECT_EQ(touch.initialize().error(),std::make_error_code(std::errc::timed_out));
    EXPECT_EQ(touch.read().error(),std::make_error_code(std::errc::not_connected));
    EXPECT_EQ(reset->read(),hal::gpio::Level::High);
}
TEST(TouchDisplay, DecodesFiveIndependentPointsAndValidityBits)
{
    auto bus=std::make_shared<TouchBus>();
    // ST1633i: validity, X[10:8], Y[10:8]; X low; Y low; unused byte.
    bus->points={0xB2,0xFF,0x57,0, 0x80,1,2,0, 0x00,9,9,0, 0x91,0,0,0, 0xA1,5,6,0};
    hal::device::St1633i touch(bus,0x55,std::make_shared<ResetPin>(),{});
    ASSERT_TRUE(touch.initialize()); const auto result=touch.read(); ASSERT_TRUE(result);
    EXPECT_EQ(result->firmware,0x31); EXPECT_TRUE(result->points[0].active);
    EXPECT_EQ(result->points[0].x,1023); EXPECT_EQ(result->points[0].y,599);
    EXPECT_FALSE(result->points[2].active); EXPECT_EQ(result->points[3].x,256);
    EXPECT_EQ(result->points[4].x,517); EXPECT_EQ(result->points[4].y,262);
}
TEST(TouchDisplay, RejectsControllerErrorAndPropagatesDisconnect)
{
    auto bus=std::make_shared<TouchBus>();
    hal::device::St1633i touch(bus,0x55,std::make_shared<ResetPin>(),{});
    ASSERT_TRUE(touch.initialize()); bus->status=1;
    EXPECT_EQ(touch.read().error(),std::make_error_code(std::errc::device_or_resource_busy));
    bus->status=4; EXPECT_TRUE(touch.read()); bus->failRead=true;
    EXPECT_EQ(touch.read().error(),std::make_error_code(std::errc::io_error));
}
