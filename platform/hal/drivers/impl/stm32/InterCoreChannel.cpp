#include "InterCoreChannel.hpp"

#include "hal/hal.hpp"
#include "hal/stm32/InterruptGuard.hpp"

#include <stdexcept>
#include <utility>

namespace
{
    // Both linkers reserve the same NOLOAD section. Neither startup clears it.
    // Only M7 initializes it, while M4 is still stopped. Whole-board reset is
    // required to reopen a closed endpoint; a live peer's queues are never reset.
    __attribute__((section(".intercore"), aligned(32))) hal::util::InterCoreMemory memory;
#if defined(CORE_CM4)
    constexpr std::size_t SIDE{0};
    constexpr auto INTERRUPT{HSEM2_IRQn};
#else
    constexpr std::size_t SIDE{1};
    constexpr auto INTERRUPT{HSEM1_IRQn};
#endif
    constexpr auto LOCAL_SEMAPHORE{static_cast<std::uint32_t>(SIDE + 1)};
    constexpr auto PEER_SEMAPHORE{static_cast<std::uint32_t>((1 - SIDE) + 1)};
    constexpr std::uint32_t LOCAL_MASK{1U << LOCAL_SEMAPHORE};
}

namespace hal
{
    void InterCoreChannel::initializeMemory() noexcept
    {
#if defined(CORE_CM7)
        // SRAM4 is clocked in D3 on H755 (no run-mode SRAM4 RCC gate).
        memory = {};
        std::atomic_ref(memory.version).store(util::InterCoreMemory::VERSION, std::memory_order_relaxed);
        std::atomic_ref(memory.magic).store(util::InterCoreMemory::MAGIC, std::memory_order_release);
        __DMB();
#endif
    }

    InterCoreChannel::InterCoreChannel()
    {
        const stm32::InterruptGuard guard;
        if (s_instance || std::atomic_ref(memory.magic).load(std::memory_order_acquire) != util::InterCoreMemory::MAGIC ||
            std::atomic_ref(memory.version).load(std::memory_order_relaxed) != util::InterCoreMemory::VERSION ||
            std::atomic_ref(memory.state[SIDE]).load(std::memory_order_acquire) != 0) {
            throw std::runtime_error("Inter-core channel unavailable; reset both cores together");
        }
        s_instance = this;
        std::atomic_ref(memory.state[SIDE]).store(1, std::memory_order_release);
        __HAL_HSEM_CLEAR_FLAG(LOCAL_MASK);
        HAL_NVIC_SetPriority(INTERRUPT, 6, 0);
        HAL_NVIC_EnableIRQ(INTERRUPT);
        HAL_HSEM_ActivateNotification(LOCAL_MASK);
        notifyPeer();
    }

    InterCoreChannel::~InterCoreChannel()
    {
        setNotification({});
        close();
        const stm32::InterruptGuard guard;
        HAL_HSEM_DeactivateNotification(LOCAL_MASK);
        HAL_NVIC_DisableIRQ(INTERRUPT);
        HAL_NVIC_ClearPendingIRQ(INTERRUPT);
        s_instance = nullptr;
    }

    bool InterCoreChannel::disconnected() const noexcept
    {
        return std::atomic_ref(memory.state[SIDE]).load(std::memory_order_acquire) == 2 ||
               std::atomic_ref(memory.state[1 - SIDE]).load(std::memory_order_acquire) == 2;
    }

    void InterCoreChannel::notifyPeer() noexcept
    {
        // Only this processor ever takes the peer's doorbell. Serialize local
        // senders and receivers so fast take/release cannot overlap on this core.
        const stm32::InterruptGuard guard;
        __DMB();
        if (HAL_HSEM_FastTake(PEER_SEMAPHORE) != HAL_OK) { hal::panic("HSEM doorbell unexpectedly owned"); }
        HAL_HSEM_Release(PEER_SEMAPHORE, 0);
    }

    std::expected<void, IInterCoreChannel::Error> InterCoreChannel::trySend(
      std::span<const std::byte> frame, Lane lane) noexcept
    {
        if (disconnected()) {
            return std::unexpected(Error::Disconnected);
        }
        auto result = memory.push(1 - SIDE, frame, lane);
        if (result) { notifyPeer(); }
        return result;
    }

    std::expected<std::size_t, IInterCoreChannel::Error> InterCoreChannel::tryReceive(
      std::span<std::byte> buffer, Lane lane) noexcept
    {
        if (disconnected()) {
            return std::unexpected(Error::Disconnected);
        }
        auto result = memory.pop(SIDE, buffer, lane);
        if (result) {
            notifyPeer();
            const stm32::InterruptGuard guard;
            if (m_notification) { m_notification(); }
        }
        return result;
    }

    void InterCoreChannel::setNotification(Notification callback) noexcept
    {
        const stm32::InterruptGuard guard;
        m_notification = std::move(callback);
        if (m_notification) { m_notification(); }
    }

    void InterCoreChannel::close() noexcept
    {
        const stm32::InterruptGuard guard;
        std::atomic_ref(memory.state[SIDE]).store(2, std::memory_order_release);
        notifyPeer();
        if (m_notification) { m_notification(); }
    }

    void InterCoreChannel::dispatchNotification(std::uint32_t mask) noexcept
    {
        if ((mask & LOCAL_MASK) && s_instance) {
            // HAL disabled notification before calling us. Rearm before waking
            // the worker; queue counters are authoritative, interrupts coalesce.
            HAL_HSEM_ActivateNotification(LOCAL_MASK);
            if (s_instance->m_notification) { s_instance->m_notification(); }
        }
    }
}

#if defined(CORE_CM4)
extern "C" void HSEM2_IRQHandler() { HAL_HSEM_IRQHandler(); }
#else
extern "C" void HSEM1_IRQHandler() { HAL_HSEM_IRQHandler(); }
#endif
extern "C" void HAL_HSEM_FreeCallback(std::uint32_t mask) { hal::InterCoreChannel::dispatchNotification(mask); }
