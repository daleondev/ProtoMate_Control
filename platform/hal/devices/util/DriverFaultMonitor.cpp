#include "DriverFaultMonitor.hpp"

#ifdef HAL_PLATFORM_STM32
#include "hal/stm32/InterruptGuard.hpp"
#else
#include "DriverMutex.hpp"
#endif
#include <atomic>
#include <stdexcept>

namespace hal::device::util
{
    using Fault = IStepperDriver::Fault;
    struct DriverFaultMonitor::State
    {
#ifdef HAL_PLATFORM_STM32
        struct Mutex {};
        struct Guard { explicit Guard(Mutex&) {} stm32::InterruptGuard interrupts; };
#else
        using Mutex = DriverMutex;
        using Guard = std::lock_guard<Mutex>;
#endif
        Mutex mutex;
        std::atomic_bool ready{};
        std::atomic<Fault> fault{ Fault::None };
        std::atomic<std::errc> error{};
        IStepperDriver::FaultCallback callback;
        void fail(Fault cause, std::error_code code) noexcept
        {
            const Guard guard{ mutex };
            ready.store(false);
            if (fault.load() != Fault::None) return;
            error.store(static_cast<std::errc>(code.value()));
            fault.store(cause);
            if (callback) callback();
        }
    };
    DriverFaultMonitor::DriverFaultMonitor(std::shared_ptr<IDigitalInput> input)
      : m_input{ std::move(input) }, m_state{ std::make_shared<State>() }
    {
        if (!m_input) throw std::invalid_argument("driver fault input unavailable");
        m_input->setEdgeCallback([state = m_state](gpio::Level level) noexcept {
            if (level == gpio::Level::High)
                state->fail(Fault::Input, std::make_error_code(std::errc::io_error));
        });
        checkInput();
    }
    DriverFaultMonitor::~DriverFaultMonitor()
    {
        m_input->clearEdgeCallback();
        setCallback({});
    }
    IStepperDriver::Status DriverFaultMonitor::status() const noexcept
    {
        const auto fault{ m_state->fault.load() };
        const bool active{ m_input->read() == gpio::Level::High };
        return { m_state->ready.load() && fault == Fault::None && !active,
                 active, fault != Fault::None, false, fault,
                 std::make_error_code(m_state->error.load()) };
    }
    void DriverFaultMonitor::setCallback(IStepperDriver::FaultCallback callback)
    {
        const State::Guard guard{ m_state->mutex };
        m_state->callback = std::move(callback);
        if (m_state->callback && m_state->fault.load() != Fault::None) m_state->callback();
    }
    void DriverFaultMonitor::checkInput() noexcept
    {
        if (m_input->read() == gpio::Level::High)
            fail(Fault::Input, std::make_error_code(std::errc::io_error));
    }
    void DriverFaultMonitor::fail(Fault fault, std::error_code error) noexcept { m_state->fail(fault, error); }
    void DriverFaultMonitor::unready() noexcept { m_state->ready.store(false); }
    void DriverFaultMonitor::beginRecovery() noexcept
    {
        const State::Guard guard{ m_state->mutex };
        m_state->ready.store(false);
        m_state->error.store({});
        m_state->fault.store(Fault::None);
    }
    hal::util::Result<> DriverFaultMonitor::completeRecovery() noexcept
    {
        checkInput();
        const State::Guard guard{ m_state->mutex };
        if (m_state->fault.load() != Fault::None)
            return std::unexpected(std::make_error_code(m_state->error.load()));
        m_state->ready.store(true);
        return {};
    }
}
