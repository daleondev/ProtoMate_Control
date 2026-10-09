#include "hal/devices/impl/Lan9253.hpp"
#include <gtest/gtest.h>
#include <array>
#include <map>
#include <thread>
#include <vector>

namespace
{
    using namespace std::chrono_literals;
    using Esc = hal::device::Lan9253;
    using Level = hal::gpio::Level;

    class Select final : public hal::IDigitalOutput
    {
      public:
        Level level{ Level::Low };
        std::vector<Level> changes;
        Level read() const noexcept override { return level; }
        void write(Level value) noexcept override { level = value; changes.push_back(value); }
        void toggle() noexcept override { write(hal::gpio::inverted(level)); }
    };

    class Spi final : public hal::ISpi
    {
      public:
        explicit Spi(Select& select) : cs{ select } {}
        Select& cs;
        std::map<std::uint16_t, std::uint32_t> values{
            { 0x64, 0x87654321 }, { 0x74, 0x08000000 }, { 0x50, 0x92530001 } };
        std::vector<std::array<std::uint8_t, 7>> requests;
        std::vector<std::chrono::milliseconds> timeouts;
        std::error_code failure;
        bool throws{};
        std::chrono::milliseconds delay{};
        bool emulateCsr{}, stuckCsr{}, bootRequests{};
        unsigned bootAcknowledgements{};
        std::map<std::uint16_t, std::uint32_t> csrValues;
        std::vector<std::pair<std::uint16_t, std::uint32_t>> csrWrites;

        std::uint32_t clockFrequencyHz() const noexcept override { return 937500; }
        hal::util::Result<> exchange(std::span<const std::uint8_t> tx,
                                     std::span<std::uint8_t> rx,
                                     std::chrono::milliseconds timeout) override
        {
            EXPECT_EQ(cs.read(), Level::Low);
            EXPECT_EQ(tx.size(), 7U);
            EXPECT_EQ(rx.size(), 7U);
            if (tx.size() != 7 || rx.size() != 7)
                return std::unexpected(std::make_error_code(std::errc::invalid_argument));
            std::array<std::uint8_t, 7> copy{};
            std::copy(tx.begin(), tx.end(), copy.begin());
            requests.push_back(copy);
            timeouts.push_back(timeout);
            if (throws) throw std::runtime_error("injected SPI failure");
            if (failure) return std::unexpected(failure);
            if (delay > 0ms) std::this_thread::sleep_for(delay);
            const auto address = std::uint16_t((tx[1] << 8) | tx[2]);
            if (tx[0] == 2) {
                const auto value = std::uint32_t{ tx[3] } | (std::uint32_t{ tx[4] } << 8) |
                                   (std::uint32_t{ tx[5] } << 16) | (std::uint32_t{ tx[6] } << 24);
                values[address] = value;
                if (emulateCsr && address == 0x304 && !stuckCsr) {
                    const auto target = static_cast<std::uint16_t>(value);
                    if (value & 0x40000000U) values[0x300] = csrValues[target];
                    else {
                        csrValues[target] = values[0x300];
                        csrWrites.emplace_back(target, values[0x300]);
                        if (bootRequests && target == 0x502) {
                            EXPECT_TRUE(values[0x300] == 0x100 || values[0x300] == 0x400);
                            if (++bootAcknowledgements == 2) values[0x74] |= Esc::readyMask;
                            else { csrValues[0x504] = 4; csrValues[0x502] = 0x9460; }
                        }
                    }
                    values[0x304] &= ~0x80000000U;
                }
                return {};
            }
            const auto value = values[address];
            // Command-phase receive data is undefined and must be ignored.
            rx[0] = 0xde; rx[1] = 0xad; rx[2] = 0xbe;
            for (unsigned i = 0; i < 4; ++i) rx[3 + i] = static_cast<std::uint8_t>(value >> (8U * i));
            return {};
        }
    };
}

TEST(Lan9253, OrdinarySpiFrameUsesBigEndianAddressLittleEndianDataAndReadTermination)
{
    Select cs;
    Spi bus{ cs };
    Esc esc{ bus, cs };
    EXPECT_EQ(cs.read(), Level::High);
    const auto value = esc.readSystemRegister(0x64, 25ms);
    ASSERT_TRUE(value);
    EXPECT_EQ(*value, 0x87654321U);
    ASSERT_EQ(bus.requests.size(), 1U);
    EXPECT_EQ(bus.requests[0], (std::array<std::uint8_t, 7>{ 3, 0, 0x64, 0, 0, 0, 0xff }));
    EXPECT_EQ(bus.timeouts[0], 25ms);
    EXPECT_EQ(cs.changes, (std::vector<Level>{ Level::High, Level::Low, Level::High }));
    bus.values[0x304] = 0x91abcdef;
    EXPECT_EQ(esc.readSystemRegister(0x304).value(), 0x91abcdefU);
    EXPECT_EQ(bus.requests.back()[1], 3U);
    EXPECT_EQ(bus.requests.back()[2], 4U);
}

TEST(Lan9253, InvalidAlignmentRangeAndTimeoutNeverSelectDevice)
{
    Select cs;
    Spi bus{ cs };
    Esc esc{ bus, cs };
    for (auto address : { 0U, 0x3cU, 0x65U, 0x400U, 0x3064U, 0xffffU })
        EXPECT_EQ(esc.readSystemRegister(address).error(), std::make_error_code(std::errc::invalid_argument));
    for (auto timeout : { -1ms, 0ms, 1001ms }) {
        EXPECT_EQ(esc.readSystemRegister(0x64, timeout).error(), std::make_error_code(std::errc::invalid_argument));
        EXPECT_EQ(esc.identify(timeout).error(), std::make_error_code(std::errc::invalid_argument));
    }
    EXPECT_TRUE(bus.requests.empty());
    EXPECT_EQ(cs.changes.size(), 1U);
    EXPECT_TRUE(esc.readSystemRegister(0x40));
    EXPECT_TRUE(esc.readSystemRegister(0x3fc));
}

TEST(Lan9253, TransportTimeoutAndExceptionAlwaysReleaseChipSelectAndAllowRetry)
{
    Select cs;
    Spi bus{ cs };
    Esc esc{ bus, cs };
    bus.failure = std::make_error_code(std::errc::timed_out);
    EXPECT_EQ(esc.readSystemRegister(0x64).error(), bus.failure);
    EXPECT_EQ(cs.read(), Level::High);
    EXPECT_EQ(esc.writeSystemRegister(0x300, 1).error(), bus.failure);
    EXPECT_EQ(cs.read(), Level::High);
    bus.failure.clear();
    bus.throws = true;
    EXPECT_THROW(static_cast<void>(esc.readSystemRegister(0x64)), std::runtime_error);
    EXPECT_THROW(static_cast<void>(esc.writeSystemRegister(0x300, 1)), std::runtime_error);
    EXPECT_EQ(cs.read(), Level::High);
    bus.throws = false;
    EXPECT_EQ(esc.readSystemRegister(0x64).value(), 0x87654321U);
}

TEST(Lan9253, IdentificationNeverReadsOtherRegistersBeforeByteTestOrIdBeforeReady)
{
    Select cs;
    Spi bus{ cs };
    Esc esc{ bus, cs };
    for (const auto bad : { 0U, 0xffffffffU, 0x21436587U }) {
        bus.requests.clear();
        bus.values[0x64] = bad;
        EXPECT_EQ(esc.identify().error(), std::make_error_code(std::errc::protocol_error));
        ASSERT_EQ(bus.requests.size(), 1U);
        EXPECT_EQ(bus.requests.front()[2], 0x64U);
    }
    bus.values[0x64] = 0x87654321;
    bus.values[0x74] = 0;
    bus.requests.clear();
    EXPECT_EQ(esc.identify().error(), std::make_error_code(std::errc::resource_unavailable_try_again));
    ASSERT_EQ(bus.requests.size(), 2U);
    EXPECT_EQ(bus.requests.back()[2], 0x74U);
    bus.values[0x74] = 0x08000000;
    bus.values[0x50] = 0x92520001;
    EXPECT_EQ(esc.identify().error(), std::make_error_code(std::errc::no_such_device));
    bus.values[0x50] = 0x92530002;
    const auto identity = esc.identify();
    ASSERT_TRUE(identity);
    EXPECT_EQ(*identity, (Esc::Identity{ 0x87654321, 0x08000000, 0x92530002 }));
    EXPECT_EQ(cs.read(), Level::High);
}

TEST(Lan9253, IdentificationSharesOneDeadlineAcrossRegisterReads)
{
    Select cs;
    Spi bus{ cs };
    Esc esc{ bus, cs };
    // A late successful response must not start a fresh full timeout for HW_CFG.
    bus.delay = 20ms;
    const auto result = esc.identify(5ms);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), std::make_error_code(std::errc::timed_out));
    EXPECT_EQ(bus.requests.size(), 1U);
    EXPECT_EQ(cs.read(), Level::High);
}

TEST(Lan9253, SystemWriteAndIndirectCsrFramingAlignmentAndBusyHandshake)
{
    Select cs;
    Spi bus{ cs };
    Esc esc{ bus, cs };
    ASSERT_TRUE(esc.writeSystemRegister(0x300, 0x12345678));
    EXPECT_EQ(bus.requests.back(), (std::array<std::uint8_t, 7>{ 2, 3, 0, 0x78, 0x56, 0x34, 0x12 }));
    bus.emulateCsr = true;
    bus.csrValues[0x502] = 0xabcd9460; // Upper bits are not part of the requested word.
    EXPECT_EQ(esc.readEscRegister(0x502, 2).value(), 0x9460U);
    ASSERT_TRUE(esc.writeEscRegister(0x508, 4, 0xabcdef98));
    EXPECT_EQ(bus.csrValues[0x508], 0xabcdef98U);
    EXPECT_EQ(bus.values[0x304], 0x00040508U);
    const auto transfers = bus.requests.size();
    for (const auto& [address, size] : { std::pair{0x501, 2}, {0x502, 4}, {0x3000, 4}, {0x500, 3}, {0x500, 0} }) {
        EXPECT_FALSE(esc.readEscRegister(address, size));
        EXPECT_FALSE(esc.writeEscRegister(address, size, 0));
    }
    EXPECT_EQ(bus.requests.size(), transfers);
    ASSERT_TRUE(esc.writeEscRegister(0x1000, 4, 0x12345678));
    EXPECT_EQ(esc.readEscRegister(0x1000, 4).value(), 0x12345678U);
    ASSERT_TRUE(esc.writeEscRegister(0x2ffc, 4, 0x87654321));
    EXPECT_EQ(esc.readEscRegister(0x2ffc, 4).value(), 0x87654321U);
    bus.values[0x304] = 0x80000000;
    EXPECT_EQ(esc.writeEscRegister(0x508, 4, 0, 2ms).error(), std::errc::timed_out);
    EXPECT_EQ(bus.csrValues[0x508], 0xabcdef98U); // Busy interface was not overwritten.
    bus.values[0x304] = 0;
    bus.stuckCsr = true;
    EXPECT_EQ(esc.readEscRegister(0x502, 2, 2ms).error(), std::errc::timed_out);
    EXPECT_EQ(cs.read(), Level::High);
}

namespace
{
    constexpr std::array<std::uint8_t, 16> bootConfiguration{
        0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xe9, 0 };
}

TEST(Lan9253, EmulatedBootServesTwoEightByteRequestsBeforeStrictIdentification)
{
    Select cs;
    Spi bus{ cs };
    Esc esc{ bus, cs };
    bus.values[0x74] = 0x1b;
    bus.emulateCsr = bus.bootRequests = true;
    bus.csrValues[0x502] = 0x9160; // Emulation, busy READ; second request is RELOAD.
    const auto identity = esc.initializeEmulatedBoot(bootConfiguration);
    ASSERT_TRUE(identity);
    EXPECT_EQ(identity->hardware_config, 0x0800001bU);
    EXPECT_EQ(bus.bootAcknowledgements, 2U);
    EXPECT_EQ(bus.csrWrites, (std::vector<std::pair<std::uint16_t, std::uint32_t>>{
        {0x508, 0x80}, {0x50c, 0}, {0x502, 0x100},
        {0x508, 0}, {0x50c, 0x00e90000}, {0x502, 0x400} }));
    const auto writes = bus.csrWrites.size();
    EXPECT_TRUE(esc.initializeEmulatedBoot(bootConfiguration)); // Already ready: no writes.
    EXPECT_EQ(bus.csrWrites.size(), writes);
}

TEST(Lan9253, BootRejectsWrongCrcModeAndUnsupportedRequestsWithoutAcknowledgingThem)
{
    Select cs;
    Spi bus{ cs };
    Esc esc{ bus, cs };
    auto invalid = bootConfiguration;
    invalid[1] ^= 1;
    EXPECT_EQ(esc.initializeEmulatedBoot(invalid).error(), std::errc::invalid_argument);
    EXPECT_TRUE(bus.requests.empty());
    bus.values[0x64] = 0xffffffff;
    EXPECT_EQ(esc.initializeEmulatedBoot(bootConfiguration).error(), std::errc::protocol_error);
    ASSERT_EQ(bus.requests.size(), 1U); // No CSR writes if the SPI byte test fails.
    bus.values[0x64] = Esc::byteTestValue;
    bus.emulateCsr = true;
    bus.values[0x74] = 0x1b;
    bus.csrValues[0x502] = 0x9440; // Physical EEPROM, not emulation.
    EXPECT_EQ(esc.initializeEmulatedBoot(bootConfiguration).error(), std::errc::operation_not_supported);
    bus.csrValues[0x502] = 0x9260; // EEPROM WRITE is never serviced.
    EXPECT_EQ(esc.initializeEmulatedBoot(bootConfiguration).error(), std::errc::operation_not_supported);
    bus.csrValues[0x502] = 0x9460;
    bus.csrValues[0x504] = 8; // Beyond the deliberately limited boot header.
    EXPECT_EQ(esc.initializeEmulatedBoot(bootConfiguration).error(), std::errc::result_out_of_range);
    EXPECT_TRUE(bus.csrWrites.empty());
    bus.csrValues[0x502] = 0x1060; // No request arrives: bounded wait.
    EXPECT_EQ(esc.initializeEmulatedBoot(bootConfiguration, 5ms).error(), std::errc::timed_out);
    EXPECT_EQ(cs.read(), Level::High);
}
