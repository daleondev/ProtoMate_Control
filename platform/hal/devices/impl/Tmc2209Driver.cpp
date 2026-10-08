#include "Tmc2209Driver.hpp"
#include <algorithm>
#include <bit>
#include <stdexcept>
#include "hal/devices/detail/DriverMutex.hpp"

namespace hal::device
{
    struct Tmc2209Driver::Lock { detail::DriverMutex mutex; };
    Tmc2209Driver::~Tmc2209Driver() = default;
    Tmc2209Driver::Tmc2209Driver(std::shared_ptr<Tmc2209Bus> bus, std::uint8_t address,
                               std::shared_ptr<IDigitalInput> diag, std::shared_ptr<IDigitalOutput> enable,
                               Configuration config, Limits limits)
      : m_bus{ std::move(bus) }, m_enable{ std::move(enable) }, m_fault{ std::move(diag) },
        m_lock{ std::make_unique<Lock>() },
        m_device{ m_bus ? m_bus->m_uart : nullptr, address }, m_configuration{ config },
        m_limits{ limits }, m_microsteps{ config.microsteps }
    {
        if (!m_enable || !std::ranges::binary_search(m_bus->m_addresses, address) || !validate(config))
            throw std::invalid_argument("invalid TMC motor configuration or bus address");
    }
    util::Result<> Tmc2209Driver::validate(const Configuration& config) const
    {
        const auto run{ Tmc2209::currentScale(config.run_milliamps) };
        const auto hold{ Tmc2209::currentScale(config.hold_milliamps) };
        if (!run || !hold || config.run_milliamps > m_limits.maximum_run_milliamps ||
            config.hold_milliamps > config.run_milliamps ||
            (m_limits.hold_equals_run && config.hold_milliamps != config.run_milliamps) ||
            !std::has_single_bit(config.microsteps) || config.microsteps > 256 ||
            config.microsteps != m_microsteps || config.index_step ||
            (config.mode != Mode::SpreadCycle && config.mode != Mode::StealthChop) ||
            (config.mode == Mode::StealthChop && *run < 8))
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        return {};
    }
    IStepperDriver::Status Tmc2209Driver::status() const noexcept
    {
        auto result{ m_fault.status() };
        result.warning = m_warning.load();
        return result;
    }
    IConfigurableStepperDriver::Snapshot Tmc2209Driver::snapshot() const
    {
        const std::scoped_lock lock{ m_lock->mutex };
        return { m_configuration, m_diagnostics, m_limits, m_device.address(),
                 Tmc2209::currentMilliamps(*Tmc2209::currentScale(m_configuration.run_milliamps)),
                 Tmc2209::currentMilliamps(*Tmc2209::currentScale(m_configuration.hold_milliamps)) };
    }
    util::Result<> Tmc2209Driver::configure(const Configuration& config)
    {
        const std::scoped_lock lock{ m_lock->mutex };
        if (m_enable->read() != gpio::Level::High)
            return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
        if (auto result{ validate(config) }; !result) return result;
        m_configuration = config;
        m_initialized = false;
        m_fault.unready();
        return {};
    }
    util::Result<> Tmc2209Driver::initialize()
    {
        const std::scoped_lock lock{ m_lock->mutex };
        if (m_enable->read() != gpio::Level::High)
            return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
        m_initialized = false;
        m_diagnostics.reset();
        m_warning.store(false);
        m_fault.beginRecovery();
        m_fault.checkInput();
        const Tmc2209Bus::Guard bus_lock{ *m_bus };
        auto result{ m_bus->prepareLocked() };
        if (result) result = m_device.initialize(m_configuration);
        if (!result) {
            m_fault.fail(result.error() == std::errc::io_error ? Fault::Electrical : Fault::Communication,
                         result.error());
            return result;
        }
        m_initialized = true;
        inspectLocked();
        return m_fault.completeRecovery();
    }
    void Tmc2209Driver::inspectLocked()
    {
        // Both the per-driver and shared-bus locks are held; callbacks do not
        // re-enter them. Retain the last diagnostic snapshot on a UART failure.
        auto state{ m_device.status() };
        if (!state) {
            m_fault.fail(Fault::Communication, state.error());
            return;
        }
        m_diagnostics = *state;
        m_warning.store(state->warning());
        if (state->reset() || state->fault()) {
            m_fault.fail(state->reset() ? Fault::Reset : Fault::Electrical,
                         std::make_error_code(std::errc::state_not_recoverable));
            return;
        }
        if (auto result{ m_device.verify() }; !result)
            m_fault.fail(result.error() == std::errc::state_not_recoverable
                           ? Fault::Configuration : Fault::Communication, result.error());
    }
    void Tmc2209Driver::service()
    {
        m_fault.checkInput();
        const std::scoped_lock lock{ m_lock->mutex };
        if (!m_initialized) return;
        const Tmc2209Bus::Guard bus_lock{ *m_bus };
        inspectLocked();
    }
    util::Result<> Tmc2209Driver::verify()
    {
        service();
        const auto state{ status() };
        if (!state.ready)
            return std::unexpected(state.error ? state.error : std::make_error_code(std::errc::operation_not_permitted));
        return {};
    }
}
