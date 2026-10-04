#include "Notification.hpp"
#include "runtime/threadx/Support.hpp"

#if defined(HAL_PLATFORM_STM32)
#include "hal/hal.hpp"
#include <tx_thread.h>
#endif

#include <stdexcept>

namespace runtime
{
    Notification::Notification()
    {
        if (!threadx::in_thread_context() ||
            tx_event_flags_create(&m_event, const_cast<char*>("notification")) != TX_SUCCESS) {
            throw std::runtime_error("notification creation failed");
        }
    }
    Notification::~Notification()
    {
        const auto status{ tx_event_flags_delete(&m_event) };
        if (status != TX_SUCCESS)
            threadx::fatal_error("notification delete", status);
    }
    void Notification::signal() noexcept
    {
#if defined(HAL_PLATFORM_STM32)
        // ThreadX's normal thread-context preemption path temporarily enables
        // interrupts. Never enter it inside the STEP generator's PRIMASK lock.
        const auto posture{ tx_interrupt_control(TX_INT_DISABLE) };
        _tx_thread_preempt_disable = _tx_thread_preempt_disable + 1U;
#endif
        const auto status{ tx_event_flags_set(&m_event, 1U, TX_OR) };
#if defined(HAL_PLATFORM_STM32)
        _tx_thread_preempt_disable = _tx_thread_preempt_disable - 1U;
        if (_tx_thread_preempt_disable == 0U && _tx_thread_current_ptr != _tx_thread_execute_ptr) {
            SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
        }
        static_cast<void>(tx_interrupt_control(posture));
#endif
        if (status != TX_SUCCESS)
            threadx::fatal_error("notification signal", status);
    }
    void Notification::clear()
    {
        ULONG flags{};
        const auto status{ tx_event_flags_get(&m_event, 1U, TX_OR_CLEAR, &flags, TX_NO_WAIT) };
        if (status != TX_SUCCESS && status != TX_NO_EVENTS) {
            threadx::fatal_error("notification clear", status);
        }
    }
    bool Notification::waitUntil(std::chrono::steady_clock::time_point deadline)
    {
        while (true) {
            const auto now{ std::chrono::steady_clock::now() };
            const auto ticks{
                deadline == std::chrono::steady_clock::time_point::max() ? TX_WAIT_FOREVER
                : deadline <= now
                  ? TX_NO_WAIT
                  : threadx::duration_to_ticks(static_cast<std::uint64_t>(
                      std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count()))
            };
            ULONG flags{};
            const auto status{ tx_event_flags_get(&m_event, 1U, TX_OR_CLEAR, &flags, ticks) };
            if (status == TX_SUCCESS)
                return true;
            if (status != TX_NO_EVENTS)
                threadx::fatal_error("notification wait", status);
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            // A tick-based timeout may return just before a steady-clock deadline.
        }
    }
}
