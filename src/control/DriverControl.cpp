#include "MotionController.hpp"
#include "pneumo/pneumo.hpp"
#include <stdexcept>

namespace control
{
    namespace
    {
        using namespace std::chrono_literals;
    }

    std::array<MotionController::DriverStatus, 2> MotionController::driverStatus() const
    {
        std::scoped_lock lock{ m_mutex };
        std::array<DriverStatus, 2> result{};
        for (std::size_t i{}; i < result.size(); ++i)
            result[i] = { static_cast<MotorId>(i + 1),
                          static_cast<std::uint8_t>(i),
                          m_driverConfigurations[i],
                          m_driverStatus[i],
                          m_driverErrors[i],
                          m_driversReady,
                          bool(m_driverFaults.load() & (1U << i)) };
        return result;
    }

    void MotionController::initializeDriversLocked()
    {
        if (m_enable->read() != hal::gpio::Level::High)
            throw std::runtime_error("disable drivers before configuring UART drivers");
        stopLocked(std::nullopt);
        for (auto& motor : m_motors)
            motor->invalidateReference();
        m_driversReady = false;
        m_driverFaults.store(0);
        constexpr std::array<std::uint8_t, 2> addresses{ 0, 1 };
        if (auto result = hal::device::Tmc2209::prepareBus(*m_driverBus, addresses); !result) {
            m_driverFaults.store(3);
            m_driverErrors.fill(result.error().message());
            throw std::runtime_error("TMC2209 UART bus preparation failed");
        }
        bool failed{};
        for (std::size_t i{}; i < 2; ++i) {
            m_driverErrors[i].clear();
            m_driverStatus[i].reset();
            auto result = m_drivers[i]->initialize(m_driverConfigurations[i]);
            if (!result) {
                m_driverErrors[i] = result.error().message();
                m_driverFaults.fetch_or(1U << i);
                failed = true;
                continue;
            }
            auto state = m_drivers[i]->status();
            if (state)
                m_driverStatus[i] = *state;
            if (!state || state->fault() || state->reset() ||
                m_diagnostics[i]->read() == hal::gpio::Level::High) {
                m_driverErrors[i] = "driver fault/DIAG asserted after initialization";
                m_driverFaults.fetch_or(1U << i);
                failed = true;
            }
        }
        if (failed || m_driverFaults.load())
            throw std::runtime_error("TMC2209 initialization failed; inspect 'motor driver status'");
        m_driversReady = true;
    }

    void MotionController::initializeDrivers()
    {
        std::scoped_lock lock{ m_mutex };
        initializeDriversLocked();
    }

    void MotionController::configureDriver(MotorId motor, DriverConfiguration configuration)
    {
        std::scoped_lock lock{ m_mutex };
        if (motor != MotorId::Motor2 && motor != MotorId::Motor3)
            throw std::invalid_argument("only m2 and m3 have UART configuration");
        if (m_enable->read() != hal::gpio::Level::High)
            throw std::runtime_error("disable drivers before changing driver settings");
        const auto i = static_cast<std::size_t>(motor) - 1U;
        // Conservative ceilings keep sine-wave PEAK current below the motor
        // drawings' 1.0 A (M2) and 0.84 A (M3) phase ratings.
        const auto limit = i == 0 ? 700U : 590U;
        auto run = hal::device::Tmc2209::currentScale(configuration.run_milliamps);
        auto hold = hal::device::Tmc2209::currentScale(configuration.hold_milliamps);
        if (!run || !hold || configuration.run_milliamps > limit ||
            configuration.hold_milliamps > configuration.run_milliamps ||
            configuration.microsteps != m_configuration[i + 1].microsteps || configuration.index_step ||
            (configuration.mode != hal::device::Tmc2209::Mode::SpreadCycle &&
             configuration.mode != hal::device::Tmc2209::Mode::StealthChop) ||
            (configuration.mode == hal::device::Tmc2209::Mode::StealthChop && *run < 8) ||
            (i == 1 && configuration.hold_milliamps != configuration.run_milliamps))
            throw std::invalid_argument(
              "invalid current/microsteps/mode; Z hold must equal run, StealthChop needs >=512 mA");
        m_driverConfigurations[i] = configuration;
        initializeDriversLocked();
    }

    void MotionController::driverFaultLocked(std::size_t index, std::string reason)
    {
        m_driverFaults.fetch_or(1U << index);
        m_driversReady = false;
        m_enable->write(hal::gpio::Level::High);
        stopLocked(std::nullopt);
        for (auto& motor : m_motors)
            motor->invalidateReference();
        m_driverErrors[index] = std::move(reason);
        // Capture the electrical cause after stopping; a DIAG edge may have
        // arrived before the next scheduled status read. Preserve the latch
        // and previous sample if communication itself has failed.
        if (auto state = m_drivers[index]->status(); state)
            m_driverStatus[index] = *state;
        pnm::log::error("M{} driver fault: {}. All motions stopped; references invalidated.",
                        index + 2,
                        m_driverErrors[index]);
    }

    void MotionController::monitorDrivers(std::stop_token stop)
    {
        while (!stop.stop_requested()) {
            m_driverNotification.waitUntil(std::chrono::steady_clock::now() + 100ms);
            if (stop.stop_requested())
                return;
            std::scoped_lock lock{ m_mutex };
            if (!m_driversReady)
                continue;
            for (std::size_t i{}; i < 2; ++i) {
                if ((m_driverFaults.load() & (1U << i)) ||
                    m_diagnostics[i]->read() == hal::gpio::Level::High) {
                    driverFaultLocked(i, "DIAG asserted");
                    break;
                }
                auto state = m_drivers[i]->status();
                const bool warned = m_driverStatus[i] && m_driverStatus[i]->warning();
                if (!state) {
                    driverFaultLocked(i, state.error().message());
                    break;
                }
                m_driverStatus[i] = *state;
                if (state->reset() || state->fault()) {
                    driverFaultLocked(i, "reset or electrical fault");
                    break;
                }
                auto verified = m_drivers[i]->verify();
                if (!verified || (m_driverFaults.load() & (1U << i)) ||
                    m_diagnostics[i]->read() == hal::gpio::Level::High) {
                    driverFaultLocked(i, !verified ? verified.error().message() : "DIAG asserted");
                    break;
                }
                if (state->warning() && !warned)
                    pnm::log::warn("M{} TMC2209 overtemperature prewarning", i + 2);
            }
        }
    }
}
