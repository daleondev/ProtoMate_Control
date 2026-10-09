#include "hal/board/board.hpp"
#include "hal/devices/impl/Tmc2209Driver.hpp"
#include "hal/drivers/impl/linux/Gpio.hpp"
#include "hal/devices/impl/linux/Tmc2209Model.hpp"

#include <gtest/gtest.h>
#include <atomic>
#include <thread>

namespace
{
    using Driver = hal::device::IStepperDriver;
    using Control = hal::device::IConfigurableStepperDriver;
    using enum hal::gpio::Level;
    using namespace std::chrono_literals;
    class StepperDrivers : public testing::Test
    {
      protected:
        std::shared_ptr<hal::device::Tmc2209Model> uart{ std::make_shared<hal::device::Tmc2209Model>() };
        std::shared_ptr<hal::IDigitalOutput> enable{ hal::board::createSteppersEnableOutput() };
        std::array<std::shared_ptr<Driver>, 3> drivers{
          hal::board::createStepperDrivers({ 16, 16, 16 }, enable, uart) };
        std::array<std::shared_ptr<hal::GpioInput>, 3> inputs{
            hal::gpio::simulatedInput({ hal::gpio::Port::F, 2 }),
            hal::gpio::simulatedInput({ hal::gpio::Port::D, 4 }),
            hal::gpio::simulatedInput({ hal::gpio::Port::D, 3 }) };
        void SetUp() override
        {
            ASSERT_TRUE(enable);
            ASSERT_TRUE(uart);
            for (auto& driver : drivers) ASSERT_TRUE(driver);
            inputs[0]->setSimulatedLevel(Low);
        }
        void TearDown() override
        {
            for (auto& driver : drivers) if (driver) driver->clearFaultCallback();
            drivers = {};
            EXPECT_EQ(uart.use_count(), 1L); // All device/bus ownership must be released.
            inputs = {};
            uart.reset();
            EXPECT_EQ(enable.use_count(), 1L);
        }
        void initialize()
        {
            for (auto& driver : drivers) ASSERT_TRUE(driver->initialize());
        }
    };
}

TEST_F(StepperDrivers, BoardSelectsRequiredCapabilitiesAndDoesNotEnableOrInitialize)
{
    EXPECT_EQ(drivers[0]->name(), "DM542T");
    EXPECT_FALSE(drivers[0]->configuration());
    EXPECT_TRUE(drivers[0]->status().fault_latched); // Open ALM at construction.
    for (const auto& driver : drivers) EXPECT_FALSE(driver->status().ready);
    auto m2{ drivers[1]->configuration()->snapshot() };
    auto m3{ drivers[2]->configuration()->snapshot() };
    EXPECT_EQ(m2.address, 0);
    EXPECT_EQ(m3.address, 1);
    EXPECT_EQ(m2.configuration.mode, Control::Mode::StealthChop);
    EXPECT_EQ(m3.configuration.mode, Control::Mode::StealthChop);
    EXPECT_EQ(m2.configuration.run_milliamps, 650);
    EXPECT_EQ(m3.configuration.run_milliamps, 550);
    EXPECT_EQ(enable->read(), High);
    initialize();
    for (auto& driver : drivers) EXPECT_TRUE(driver->status().ready);
    EXPECT_EQ(enable->read(), High); // Recovery never enables.
}

TEST_F(StepperDrivers, AlmAndDiagLatchTransientEdgesUntilDisabledRecovery)
{
    initialize();
    for (unsigned i{}; i < drivers.size(); ++i) {
        unsigned callbacks{};
        drivers[i]->setFaultCallback([&]() noexcept { ++callbacks; });
        inputs[i]->setSimulatedLevel(High);
        EXPECT_EQ(callbacks, 1U);
        EXPECT_TRUE(drivers[i]->status().fault_active);
        inputs[i]->setSimulatedLevel(Low);
        EXPECT_FALSE(drivers[i]->status().fault_active);
        EXPECT_TRUE(drivers[i]->status().fault_latched);
        EXPECT_EQ(drivers[i]->status().fault, Driver::Fault::Input);
        EXPECT_FALSE(drivers[i]->verify());
        EXPECT_FALSE(drivers[i]->status().ready);
        enable->write(Low);
        EXPECT_FALSE(drivers[i]->initialize());
        EXPECT_EQ(enable->read(), Low); // Driver itself does not own shared shutdown.
        enable->write(High);
        ASSERT_TRUE(drivers[i]->initialize());
        EXPECT_TRUE(drivers[i]->status().ready);
        inputs[i]->setSimulatedLevel(High);
        EXPECT_EQ(callbacks, 2U);
        EXPECT_FALSE(drivers[i]->initialize()); // Active input cannot be acknowledged away.
        drivers[i]->clearFaultCallback();
        inputs[i]->setSimulatedLevel(Low);
    }
}

TEST_F(StepperDrivers, ConfigurationPreservesMotorScalingCurrentLimitsAndDisabledState)
{
    initialize();
    auto& control{ *drivers[1]->configuration() };
    const auto original{ control.snapshot().configuration };
    auto invalid{ original };
    invalid.microsteps = 32;
    EXPECT_FALSE(control.configure(invalid));
    invalid = original;
    invalid.run_milliamps = 701;
    EXPECT_FALSE(control.configure(invalid));
    invalid = original;
    invalid.index_step = true;
    EXPECT_FALSE(control.configure(invalid));
    EXPECT_EQ(control.snapshot().configuration, original);
    EXPECT_TRUE(drivers[1]->status().ready);
    auto& z{ *drivers[2]->configuration() };
    invalid = z.snapshot().configuration;
    invalid.hold_milliamps = 400;
    EXPECT_FALSE(z.configure(invalid));
    auto changed{ original };
    changed.run_milliamps = changed.hold_milliamps = 600;
    changed.mode = Control::Mode::SpreadCycle;
    enable->write(Low);
    EXPECT_FALSE(control.configure(changed));
    enable->write(High);
    ASSERT_TRUE(control.configure(changed));
    EXPECT_FALSE(drivers[1]->status().ready);
    EXPECT_FALSE(drivers[1]->verify());
    ASSERT_TRUE(drivers[1]->initialize());
    EXPECT_TRUE(drivers[1]->status().ready);
    EXPECT_EQ(control.snapshot().configuration, changed);
    EXPECT_LE(control.snapshot().nominal_run_milliamps, 600);
    EXPECT_EQ(uart->getRegister(0, 0x00), 0x1C4U);
    EXPECT_EQ(uart->getRegister(1, 0x00), 0x1C0U);
}

TEST_F(StepperDrivers, UartFailuresAndResetUseTheCommonFaultCallbackWithoutFallback)
{
    initialize();
    unsigned callbacks{};
    drivers[1]->setFaultCallback([&]() noexcept { ++callbacks; });
    auto before{ drivers[1]->configuration()->snapshot().diagnostics };
    uart->setConnected(0, false);
    drivers[1]->service();
    EXPECT_EQ(callbacks, 1U);
    EXPECT_EQ(drivers[1]->status().fault, Driver::Fault::Communication);
    EXPECT_EQ(drivers[1]->configuration()->snapshot().diagnostics->driver, before->driver);
    EXPECT_FALSE(drivers[1]->initialize());
    EXPECT_FALSE(drivers[1]->status().ready);
    EXPECT_TRUE(drivers[0]->verify()); // DM542T does not require UART.
    uart->setConnected(0, true);
    drivers[1]->service();
    EXPECT_FALSE(drivers[1]->status().ready); // A successful read cannot recover a latch.
    ASSERT_TRUE(drivers[1]->initialize());
    uart->reset(0);
    drivers[1]->service();
    EXPECT_EQ(drivers[1]->status().fault, Driver::Fault::Reset);
    EXPECT_FALSE(drivers[1]->status().ready);
    ASSERT_TRUE(drivers[1]->initialize());
    uart->setRegister(0, 0x6C, 0);
    drivers[1]->service();
    EXPECT_EQ(drivers[1]->status().fault, Driver::Fault::Configuration);
}

TEST_F(StepperDrivers, WarningsRemainDiagnosticsAndElectricalFaultsBlockReadiness)
{
    initialize();
    uart->setRegister(1, 0x6F, 0x800000C1);
    drivers[2]->service();
    EXPECT_TRUE(drivers[2]->status().ready);
    EXPECT_TRUE(drivers[2]->status().warning);
    EXPECT_TRUE(drivers[2]->configuration()->snapshot().diagnostics->openLoad());
    uart->setRegister(1, 0x6F, 2);
    drivers[2]->service();
    EXPECT_EQ(drivers[2]->status().fault, Driver::Fault::Electrical);
    EXPECT_FALSE(drivers[2]->status().ready);
}

TEST_F(StepperDrivers, DestructionDisconnectsConcurrentFaultEdges)
{
    initialize();
    std::atomic_uint callbacks{};
    drivers[1]->setFaultCallback([&]() noexcept { ++callbacks; });
    std::jthread edges{ [&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            inputs[1]->setSimulatedLevel(High);
            inputs[1]->setSimulatedLevel(Low);
        }
    } };
    std::this_thread::sleep_for(5ms);
    drivers[1].reset();
    const auto stopped{ callbacks.load() };
    std::this_thread::sleep_for(5ms);
    EXPECT_EQ(callbacks.load(), stopped);
}

TEST(StepperDriverBus, PreparesAllNodesSerializesDriverOperationsAndRetainsAnInitializationEdge)
{
    class Bus final : public hal::IUart
    {
      public:
        hal::device::Tmc2209Model model;
        std::atomic_bool entered{}, overlap{};
        std::atomic_uint prepared{};
        std::function<void()> on_read;
        hal::util::Result<> exchange(std::span<const std::uint8_t> tx, std::span<std::uint8_t> rx,
                                     std::chrono::milliseconds timeout) override
        {
            if (entered.exchange(true)) overlap = true;
            if (tx.size() == 8 && tx[2] == 0x83) prepared.fetch_or(1U << tx[1]);
            if (!rx.empty()) {
                EXPECT_EQ(prepared.load(), 3U);
                if (on_read) { auto hook{ std::exchange(on_read, {}) }; hook(); }
            }
            std::this_thread::yield();
            auto result{ model.exchange(tx, rx, timeout) };
            entered = false;
            return result;
        }
    };
    auto transport{ std::make_shared<Bus>() };
    auto bus{ std::make_shared<hal::device::Tmc2209Bus>(transport, std::vector<std::uint8_t>{0, 1}) };
    auto enable{ hal::board::createSteppersEnableOutput() };
    auto diag2{ hal::board::createStepperDiagnostic(hal::board::MotorId::Motor2) };
    auto diag3{ hal::board::createStepperDiagnostic(hal::board::MotorId::Motor3) };
    auto input{ hal::gpio::simulatedInput({ hal::gpio::Port::D, 4 }) };
    hal::device::Tmc2209Driver m2{ bus, 0, diag2, enable, {}, {700, false} };
    hal::device::Tmc2209Driver m3{ bus, 1, diag3, enable,
        { .run_milliamps=550, .hold_milliamps=550 }, {590, true} };
    transport->on_read = [&] { input->setSimulatedLevel(High); input->setSimulatedLevel(Low); };
    EXPECT_FALSE(m2.initialize());
    EXPECT_TRUE(m2.status().fault_latched);
    EXPECT_FALSE(m2.status().ready);
    std::jthread first{ [&] { EXPECT_TRUE(m2.initialize()); for (int i{}; i<20; ++i) m2.service(); } };
    std::jthread second{ [&] { EXPECT_TRUE(m3.initialize()); for (int i{}; i<20; ++i) m3.service(); } };
    first.join();
    second.join();
    EXPECT_FALSE(transport->overlap.load());
    EXPECT_TRUE(m2.status().ready);
    EXPECT_TRUE(m3.status().ready);
}
