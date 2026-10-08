#include "MotionController.hpp"
#include "pneumo/pneumo.hpp"
#include <algorithm>
#include <stdexcept>

namespace control
{
    bool MotionController::driversReady() const noexcept
    {
        return std::ranges::all_of(m_motors, [](const auto& motor) { return motor->driver().status().ready; });
    }

    std::array<MotionController::DriverStatus, 3> MotionController::driverStatus() const
    {
        std::scoped_lock lock{ m_mutex };
        std::array<DriverStatus, 3> result{};
        for (std::size_t i{}; i < result.size(); ++i) {
            auto& driver{ m_motors[i]->driver() };
            auto* control{ driver.configuration() };
            result[i] = { static_cast<MotorId>(i), driver.name(), driver.status(),
                          control ? std::optional{ control->snapshot() } : std::nullopt };
        }
        return result;
    }

    void MotionController::initializeDriversLocked()
    {
        if (m_enable->read() != hal::gpio::Level::High)
            throw std::runtime_error("disable drivers before initialization");
        stopLocked(std::nullopt);
        for (auto& motor : m_motors) motor->invalidateReference();
        m_driverFaults.store(0);
        m_handledDriverFaults = 0;
        for (auto& motor : m_motors) static_cast<void>(motor->driver().initialize());
        handleDriverFaultsLocked();
        if (!driversReady() || m_driverFaults.load())
            throw std::runtime_error("driver initialization failed; inspect 'motor driver status'");
    }

    void MotionController::initializeDrivers()
    {
        std::scoped_lock lock{ m_mutex };
        initializeDriversLocked();
    }

    void MotionController::configureDriver(MotorId id, DriverConfiguration configuration)
    {
        std::scoped_lock lock{ m_mutex };
        auto* capability{ axis(id).driver().configuration() };
        if (!capability) throw std::invalid_argument("driver configuration uses hardware switches; no programmable capability");
        if (m_enable->read() != hal::gpio::Level::High)
            throw std::runtime_error("disable drivers before changing driver settings");
        if (auto result{ capability->configure(configuration) }; !result)
            throw std::invalid_argument("driver configuration rejected: " + result.error().message());
        initializeDriversLocked();
    }

    void MotionController::handleDriverFaultsLocked()
    {
        for (std::size_t i{}; i < m_motors.size(); ++i) {
            const auto status{ m_motors[i]->driver().status() };
            if (status.fault_active || status.fault_latched) m_driverFaults.fetch_or(1U << i);
        }
        const auto faults{ m_driverFaults.load() };
        if (!faults) return;
        m_enable->write(hal::gpio::Level::High);
        const auto newly_handled{ faults & ~m_handledDriverFaults };
        if (!newly_handled) return;
        stopLocked(std::nullopt);
        for (auto& motor : m_motors) motor->invalidateReference();
        m_handledDriverFaults |= faults;
        for (std::size_t i{}; i < m_motors.size(); ++i) {
            if (!(newly_handled & (1U << i))) continue;
            auto& driver{ m_motors[i]->driver() };
            pnm::log::error("M{} {} fault: {}. All motions stopped; references invalidated.",
                            i + 1, driver.name(), driver.status().error.message());
        }
    }

    void MotionController::monitorDrivers(std::stop_token stop)
    {
        using namespace std::chrono_literals;
        while (!stop.stop_requested()) {
            m_driverNotification.waitUntil(std::chrono::steady_clock::now() + 100ms);
            if (stop.stop_requested()) return;
            std::scoped_lock lock{ m_mutex };
            // Handle GPIO shutdown before potentially blocking UART operations.
            handleDriverFaultsLocked();
            for (std::size_t i{}; i < m_motors.size(); ++i) {
                auto& driver{ m_motors[i]->driver() };
                const bool warned{ driver.status().warning };
                driver.service();
                handleDriverFaultsLocked();
                if (driver.status().warning && !warned)
                    pnm::log::warn("M{} {} overtemperature prewarning", i + 1, driver.name());
            }
        }
    }
}
