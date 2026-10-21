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

#include "hal/devices/impl/SmartwinDisplay.hpp"
#include <thread>
namespace {
class SupplyReady final : public hal::IDigitalInput {
public:
    bool ready{};
    auto read() const noexcept -> hal::gpio::Level override
    { return ready ? hal::gpio::Level::High : hal::gpio::Level::Low; }
    void setEdgeCallback(EdgeCallback) noexcept override {}
};
class DisplayOutput final : public hal::IRgbOutput {
public:
    unsigned brightness{};
    bool initialized{};
    hal::display::State current{};
    auto initialize() -> hal::util::Result<> override { initialized=true;return {}; }
    auto framebuffer(unsigned) -> std::span<std::uint16_t> override { return {}; }
    auto present(unsigned) -> hal::util::Result<> override { ++current.presentations;return {}; }
    auto requestPresent(unsigned slot) -> hal::util::Result<> override { return present(slot); }
    auto state() const noexcept -> hal::display::State override { return current; }
    void setCallback(hal::display::Callback) noexcept override {}
    void shutdown() noexcept override { initialized=false;brightness=0; }
    auto fill(unsigned,std::uint16_t) -> hal::util::Result<> override { return {}; }
    auto backlight(unsigned percent) -> hal::util::Result<> override { brightness=percent;return {}; }
    auto underruns() const noexcept -> std::uint32_t override { return 0; }
    auto width() const noexcept -> unsigned override { return 1024; }
    auto height() const noexcept -> unsigned override { return 600; }
};
void startPanel(hal::device::SmartwinDisplay& display)
{
    using namespace std::chrono_literals;
    display.service();
    std::this_thread::sleep_for(3ms);display.service();
    std::this_thread::sleep_for(11ms);display.service();
    std::this_thread::sleep_for(61ms);display.service();
}
}
TEST(TouchDisplay, SupplyLossGatesBrightnessAndRecoveryRepeatsResetWithoutStoppingRendering)
{
    auto rgb=std::make_shared<DisplayOutput>();auto standby=std::make_shared<ResetPin>();
    auto reset=std::make_shared<ResetPin>();auto power=std::make_shared<SupplyReady>();
    hal::device::SmartwinDisplay display(rgb,standby,reset,power);
    ASSERT_TRUE(display.initialize());ASSERT_TRUE(display.backlight(65));
    EXPECT_TRUE(rgb->initialized);EXPECT_EQ(rgb->brightness,0u);
    EXPECT_EQ(reset->value,hal::gpio::Level::Low);
    power->ready=true;startPanel(display);
    EXPECT_EQ(reset->value,hal::gpio::Level::High);EXPECT_EQ(standby->value,hal::gpio::Level::High);
    EXPECT_EQ(rgb->brightness,0u); // A valid framebuffer and a scan boundary are required.
    rgb->current.presentations=1;++rgb->current.vsyncs;display.service();
    EXPECT_EQ(rgb->brightness,65u);
    power->ready=false;display.service();
    EXPECT_TRUE(rgb->initialized);EXPECT_EQ(rgb->brightness,0u);
    EXPECT_EQ(reset->value,hal::gpio::Level::Low);EXPECT_EQ(standby->value,hal::gpio::Level::Low);
    power->ready=true;startPanel(display);EXPECT_EQ(rgb->brightness,0u);
    ++rgb->current.vsyncs;display.service(); // Static scene is already valid; no new submission needed.
    EXPECT_EQ(rgb->brightness,65u);
    display.shutdown();EXPECT_FALSE(rgb->initialized);EXPECT_EQ(rgb->brightness,0u);
    display.service();EXPECT_EQ(reset->value,hal::gpio::Level::Low);
}
TEST(TouchDisplay, BrownoutDuringStartupRestartsTheSequence)
{
    auto rgb=std::make_shared<DisplayOutput>();auto standby=std::make_shared<ResetPin>();
    auto reset=std::make_shared<ResetPin>();auto power=std::make_shared<SupplyReady>();power->ready=true;
    hal::device::SmartwinDisplay display(rgb,standby,reset,power);
    ASSERT_TRUE(display.initialize());ASSERT_TRUE(display.backlight(10));
    std::this_thread::sleep_for(std::chrono::milliseconds{3});display.service();
    EXPECT_EQ(reset->value,hal::gpio::Level::High);
    power->ready=false;display.service();EXPECT_EQ(reset->value,hal::gpio::Level::Low);
    EXPECT_EQ(standby->value,hal::gpio::Level::Low);EXPECT_EQ(rgb->brightness,0u);
    EXPECT_FALSE(display.backlight(101));
}
