#include "hal/devices/impl/Tmc2209.hpp"
#include "hal/drivers/impl/linux/TmcUart.hpp"
#include <gtest/gtest.h>

namespace
{
    using Driver = hal::device::Tmc2209;
    class FaultyBus : public hal::IUart
    {
      public:
        hal::uart::TmcUart model;
        bool corrupt{}, wrong_register{}, drop_write{};
        hal::util::Result<> exchange(std::span<const std::uint8_t> tx,
                                     std::span<std::uint8_t> rx,
                                     std::chrono::milliseconds timeout) override
        {
            if (drop_write && tx.size() == 8 && tx[2] != 0x83)
                return {};
            auto result = model.exchange(tx, rx, timeout);
            if (result && !rx.empty()) {
                if (corrupt)
                    rx.back() ^= 1;
                if (wrong_register) {
                    rx[2] ^= 1;
                    rx.back() = Driver::crc(rx.first(7));
                }
            }
            return result;
        }
    };
}

TEST(Tmc2209, DatasheetCrcVectorsAndConservativeCurrentQuantization)
{
    EXPECT_EQ(Driver::crc(std::array<std::uint8_t, 3>{ 5, 0, 0 }), 0x48);
    EXPECT_EQ(Driver::crc(std::array<std::uint8_t, 3>{ 5, 0, 6 }), 0x6F);
    for (std::uint16_t current = 100; current <= 1800; ++current) {
        auto scale = Driver::currentScale(current);
        ASSERT_TRUE(scale);
        EXPECT_LE(*scale, 31);
        EXPECT_LE(Driver::currentMilliamps(*scale), current);
        EXPECT_LT(current - Driver::currentMilliamps(*scale), 58);
    }
    EXPECT_FALSE(Driver::currentScale(0));
    EXPECT_FALSE(Driver::currentScale(1801));
}

TEST(Tmc2209, IndependentAddressesAndCompleteInitialization)
{
    auto bus = std::make_shared<FaultyBus>();
    bus->model.setRegister(0, 2, 255);
    Driver m2{ bus, 0 }, m3{ bus, 1 };
    ASSERT_TRUE(m2.initialize({}));
    ASSERT_TRUE(m3.initialize(
      { .run_milliamps = 550, .hold_milliamps = 550, .microsteps = 32, .mode = Driver::Mode::StealthChop }));
    EXPECT_EQ(bus->model.getRegister(0, 0), 0x1C4U); // digital current, UART, MRES, SpreadCycle
    EXPECT_EQ(bus->model.getRegister(1, 0), 0x1C0U);
    EXPECT_EQ((bus->model.getRegister(0, 0x6C) >> 24) & 15, 4);
    EXPECT_EQ((bus->model.getRegister(1, 0x6C) >> 24) & 15, 3);
    EXPECT_EQ(bus->model.getRegister(0, 0x22), 0U); // external STEP
    EXPECT_EQ(bus->model.getRegister(0, 0x42), 0U); // CoolStep off
    EXPECT_EQ(bus->model.getRegister(0, 0x14), 0U); // no DIAG stall events
    EXPECT_EQ(bus->model.getRegister(0, 3), 0x200U);
    ASSERT_TRUE(m2.verify());
    bus->model.setRegister(0, 0x6C, 0);
    EXPECT_FALSE(m2.verify());
    EXPECT_TRUE(m3.verify());
}

TEST(Tmc2209, RejectsCorruptMisdirectedMissingAndUnacknowledgedTraffic)
{
    auto bus = std::make_shared<FaultyBus>();
    Driver driver{ bus, 0 };
    bus->corrupt = true;
    EXPECT_FALSE(driver.initialize({}));
    bus->corrupt = false;
    bus->wrong_register = true;
    EXPECT_FALSE(driver.initialize({}));
    bus->wrong_register = false;
    bus->drop_write = true;
    EXPECT_FALSE(driver.initialize({}));
    bus->drop_write = false;
    bus->model.setConnected(0, false);
    EXPECT_FALSE(driver.initialize({}));
    bus->model.setConnected(0, true);
    ASSERT_TRUE(driver.initialize({}));
    bus->model.setRegister(0, 6, 0x20000001);
    EXPECT_FALSE(driver.initialize({}));
    bus->model.setRegister(0, 6, 0x21000000);
    EXPECT_FALSE(driver.initialize({})); // enabled
    bus->model.setRegister(0, 6, 0x21000101);
    EXPECT_FALSE(driver.initialize({})); // SPRD jumper closed
    bus->model.setRegister(0, 6, 0x21000005);
    EXPECT_FALSE(driver.initialize({})); // wrong straps
}

TEST(Tmc2209, DecodesFaultsWithoutTreatingOpenLoadAsReliableAtStandstill)
{
    auto bus = std::make_shared<FaultyBus>();
    Driver driver{ bus, 1 };
    ASSERT_TRUE(driver.initialize({}));
    bus->model.setRegister(1, 0x6F, 0x800000C1);
    auto status = driver.status();
    ASSERT_TRUE(status);
    EXPECT_TRUE(status->warning());
    EXPECT_TRUE(status->openLoad());
    EXPECT_TRUE(status->standstill());
    EXPECT_FALSE(status->fault());
    for (unsigned bit = 1; bit <= 5; ++bit) {
        bus->model.setRegister(1, 0x6F, 1U << bit);
        EXPECT_TRUE(driver.status()->fault());
    }
    bus->model.reset(1);
    EXPECT_TRUE(driver.status()->reset());
    EXPECT_FALSE(driver.verify());
}
