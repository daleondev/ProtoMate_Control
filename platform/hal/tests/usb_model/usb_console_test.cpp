#include "hal/console.hpp"
#include "hal/stm32/UsbConsole.h"
#include "hal/hal.hpp"
#include <tx_api.h>
#include <gtest/gtest.h>
#include <array>
#include <span>
#include <string_view>
#include <vector>

extern "C" int __io_putchar(int);
extern "C" std::uint32_t giga_console_dropped_bytes;
namespace {
void (*entry)(ULONG){};
bool pending{};
struct Idle {};
std::span<const std::uint8_t> activePacket;
std::vector<std::vector<std::uint8_t>> transfers;
void service()
{
    try { entry(0); } catch (const Idle&) {}
    // A real context switch restores the thread's interrupt state.
    usbTestPrimask=0;
}
void receive(std::string_view bytes)
{
    giga_console_receive(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
}
void emit(std::string_view bytes) { for (const auto ch : bytes) { __io_putchar(ch); } }
class UsbConsole : public testing::Test {
    void SetUp() override
    {
        hal::console::initialize();
        giga_console_deinitialized();
        transfers.clear(); activePacket={};
        giga_console_dropped_bytes=0;
        static_cast<void>(hal::console::try_read());
    }
};
}
unsigned tx_event_flags_create(TX_EVENT_FLAGS_GROUP*, char*) { return TX_SUCCESS; }
unsigned tx_event_flags_set(TX_EVENT_FLAGS_GROUP*, ULONG, unsigned) { pending=true; return TX_SUCCESS; }
unsigned tx_event_flags_get(TX_EVENT_FLAGS_GROUP*, ULONG, unsigned, ULONG* flags, ULONG)
{
    if (!pending) { throw Idle{}; }
    pending=false; *flags=1; return TX_SUCCESS;
}
unsigned tx_thread_create(TX_THREAD*, char*, void (*function)(ULONG), ULONG, void*, ULONG,
                          unsigned, unsigned, ULONG, unsigned)
{
    entry=function; return TX_SUCCESS;
}
unsigned CDC_Transmit_FS(std::uint8_t* data, std::uint16_t length)
{
    activePacket={data,length};
    transfers.emplace_back(data,data+length);
    return 0;
}
TEST_F(UsbConsole, AbsentHostNeverBlocksAndCountsDrops)
{
    emit("hello"); service();
    EXPECT_EQ(giga_console_dropped_bytes,5u);
    EXPECT_TRUE(transfers.empty());
    EXPECT_EQ(hal::console::try_read().state,hal::console::InputState::Idle);
}
TEST_F(UsbConsole, DisconnectClearsPartialInputAndCanReconnect)
{
    giga_console_connected(1); static_cast<void>(hal::console::try_read());
    receive("motor move m1 ");
    giga_console_deinitialized();
    EXPECT_EQ(hal::console::try_read().state,hal::console::InputState::Reset);
    EXPECT_EQ(hal::console::try_read().state,hal::console::InputState::Idle);
    giga_console_connected(1); static_cast<void>(hal::console::try_read());
    receive("help\n");
    for (char c : std::string_view{"help\n"}) { EXPECT_EQ(hal::console::try_read().character,c); }
}
TEST_F(UsbConsole, OverflowCannotExecuteTruncatedInput)
{
    giga_console_connected(1); static_cast<void>(hal::console::try_read());
    std::array<std::uint8_t,4096> bytes{};
    giga_console_receive(bytes.data(),bytes.size());
    receive("overflow");
    EXPECT_EQ(hal::console::try_read().state,hal::console::InputState::Reset);
    receive("discarded tail\n"); receive("x");
    EXPECT_EQ(hal::console::try_read().character,'x');
    EXPECT_EQ(hal::console::try_read().state,hal::console::InputState::Idle);
}
TEST_F(UsbConsole, OutputIsBoundedAndPacketRemainsStableUntilCompletion)
{
    giga_console_connected(1);
    for (unsigned i=0;i<8200;++i) { __io_putchar('a'); }
    EXPECT_EQ(giga_console_dropped_bytes,8u);
    service(); ASSERT_EQ(transfers.size(),1u);
    ASSERT_EQ(activePacket.size(),64u);
    emit("new data"); service(); EXPECT_EQ(transfers.size(),1u);
    EXPECT_EQ(std::vector<std::uint8_t>(activePacket.begin(),activePacket.end()),transfers[0]);
    giga_console_transmitted(); service(); EXPECT_EQ(transfers.size(),2u);
}
TEST_F(UsbConsole, SuspendResumeRestoresExistingDtrWithoutNewOpen)
{
    giga_console_connected(1); receive("partial");
    giga_console_suspend(); emit("lost");
    EXPECT_EQ(giga_console_dropped_bytes,4u);
    giga_console_resume();
    EXPECT_EQ(hal::console::try_read().state,hal::console::InputState::Reset);
    EXPECT_EQ(hal::console::try_read().state,hal::console::InputState::Idle);
    emit("ok"); service(); ASSERT_EQ(transfers.size(),1u);
    EXPECT_EQ(transfers[0],(std::vector<std::uint8_t>{'o','k'}));
}
TEST_F(UsbConsole, DeinitReleasesPacketAndCountsUnsentOutput)
{
    giga_console_connected(1); emit("pending"); service();
    giga_console_deinitialized();
    EXPECT_EQ(giga_console_dropped_bytes,7u);
    giga_console_connected(1); emit("next"); service();
    ASSERT_EQ(transfers.size(),2u);
    EXPECT_EQ(transfers[1],(std::vector<std::uint8_t>{'n','e','x','t'}));
}
