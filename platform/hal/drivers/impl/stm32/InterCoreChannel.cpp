// CMSIS/HAL register macros use fixed MMIO addresses and C pointer casts.
// NOLINTBEGIN(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
#include "InterCoreChannel.hpp"
#include "hal/util/InterCoreTrace.hpp"
#include "profiling/Trace.h"
#include "profiling/Trace.hpp"

#include "hal/hal.hpp"
#include "hal/stm32/InterruptGuard.hpp"

#include <stdexcept>
#include <utility>

namespace
{
    constexpr unsigned CACHE_LINE_BYTES{ 32 }, ICC_IRQ_PRIORITY{ 6 };
    // Both linkers reserve the same NOLOAD section. Neither startup clears it.
    // Only M7 initializes it, while M4 is still stopped. Whole-board reset is
    // required to reopen a closed endpoint; a live peer's queues are never reset.
    // Linker-placed shared SRAM ABI; atomic publication and HSEM coordinate the two cores.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    __attribute__((section(".intercore"), aligned(CACHE_LINE_BYTES))) hal::util::InterCoreMemory memory{};
#if defined(CORE_CM4)
    constexpr std::size_t SIDE{ 0 };
    constexpr auto INTERRUPT{ HSEM2_IRQn };
#else
    constexpr std::size_t SIDE{ 1 };
    constexpr auto INTERRUPT{ HSEM1_IRQn };
#endif
    constexpr auto LOCAL_SEMAPHORE{ static_cast<std::uint32_t>(SIDE + 1) };
    constexpr auto PEER_SEMAPHORE{ static_cast<std::uint32_t>((1 - SIDE) + 1) };
    constexpr std::uint32_t LOCAL_MASK{ 1U << LOCAL_SEMAPHORE };
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
        const stm32::InterruptGuard guard{};
        if (s_instance ||
            std::atomic_ref(memory.magic).load(std::memory_order_acquire) != util::InterCoreMemory::MAGIC ||
            std::atomic_ref(memory.version).load(std::memory_order_relaxed) !=
              util::InterCoreMemory::VERSION ||
            std::atomic_ref(memory.state[SIDE]).load(std::memory_order_acquire) != 0) {
            throw std::runtime_error("Inter-core channel unavailable; reset both cores together");
        }
        s_instance = this;
        std::atomic_ref(memory.state[SIDE]).store(1, std::memory_order_release);
        __HAL_HSEM_CLEAR_FLAG(LOCAL_MASK);
        HAL_NVIC_SetPriority(INTERRUPT, ICC_IRQ_PRIORITY, 0);
        HAL_NVIC_EnableIRQ(INTERRUPT);
        HAL_HSEM_ActivateNotification(LOCAL_MASK);
        notifyPeer();
    }

    InterCoreChannel::~InterCoreChannel()
    {
        setNotification({});
        close();
        const stm32::InterruptGuard guard{};
        HAL_HSEM_DeactivateNotification(LOCAL_MASK);
        HAL_NVIC_DisableIRQ(INTERRUPT);
        HAL_NVIC_ClearPendingIRQ(INTERRUPT);
        s_instance = nullptr;
    }

    bool InterCoreChannel::disconnected() noexcept
    {
        return std::atomic_ref(memory.state[SIDE]).load(std::memory_order_acquire) == 2 ||
               std::atomic_ref(memory.state[1 - SIDE]).load(std::memory_order_acquire) == 2;
    }

    void InterCoreChannel::notifyPeer() noexcept
    {
        // Only this processor ever takes the peer's doorbell. Serialize local
        // senders and receivers so fast take/release cannot overlap on this core.
        const stm32::InterruptGuard guard{};
        __DMB();
        if (HAL_HSEM_FastTake(PEER_SEMAPHORE) != HAL_OK) {
            hal::panic("HSEM doorbell unexpectedly owned");
        }
        HAL_HSEM_Release(PEER_SEMAPHORE, 0);
    }

    std::expected<void, IInterCoreChannel::Error> InterCoreChannel::trySend(std::span<const std::byte> frame,
                                                                            Lane lane) noexcept
    {
        if (disconnected()) {
            return std::unexpected(Error::Disconnected);
        }
        util::intercore_trace(util::InterCoreStage::PublishBegin, unsigned(lane), frame);
        auto result{ memory.push(1 - SIDE, frame, lane) };
#if defined(PROTOMATE_PROFILING)
        auto& q{ memory.inbox[1U - SIDE][unsigned(lane)] };
        const auto used{ std::atomic_ref(q.written).load() - std::atomic_ref(q.read).load() };
        profile_queue(unsigned(lane),
                      std::min(used, q.CAPACITY),
                      q.CAPACITY,
                      !result && result.error() == IInterCoreChannel::Error::WouldBlock,
                      0,
                      0);
#endif
        if (result) {
            util::intercore_trace(util::InterCoreStage::Published, unsigned(lane), frame);
            notifyPeer();
        }
        return result;
    }

    std::expected<std::size_t, IInterCoreChannel::Error> InterCoreChannel::tryReceive(
      std::span<std::byte> buffer,
      Lane lane) noexcept
    {
        if (disconnected()) {
            return std::unexpected(Error::Disconnected);
        }
        auto result{ memory.pop(SIDE, buffer, lane) };
#if defined(PROTOMATE_PROFILING)
        auto& q{ memory.inbox[SIDE][unsigned(lane)] };
        const auto used{ std::atomic_ref(q.written).load() - std::atomic_ref(q.read).load() };
        profile_queue(3U + unsigned(lane), std::min(used, q.CAPACITY), q.CAPACITY, 0, 0, 0);
#endif
        if (result) {
            util::intercore_trace(util::InterCoreStage::Received, unsigned(lane), buffer.first(*result));
        }
        if (result) {
            notifyPeer();
            const stm32::InterruptGuard guard{};
            if (m_notification) {
                m_notification();
            }
        }
        return result;
    }

    void InterCoreChannel::setNotification(Notification callback) noexcept
    {
        const stm32::InterruptGuard guard{};
        m_notification = std::move(callback);
        if (m_notification) {
            m_notification();
        }
    }

    IInterCoreChannel::Readiness InterCoreChannel::readiness() noexcept { return memory.readiness(SIDE); }

    void InterCoreChannel::close() noexcept
    {
        const stm32::InterruptGuard guard{};
        std::atomic_ref(memory.state[SIDE]).store(2, std::memory_order_release);
        notifyPeer();
        if (m_notification) {
            m_notification();
        }
    }

    void InterCoreChannel::dispatchNotification(std::uint32_t mask) noexcept
    {
        if ((mask & LOCAL_MASK) && s_instance) {
            util::intercore_trace(util::InterCoreStage::Interrupt);
            // HAL disabled notification before calling us. Rearm before waking
            // the worker; queue counters are authoritative, interrupts coalesce.
            HAL_HSEM_ActivateNotification(LOCAL_MASK);
            if (s_instance->m_notification) {
                s_instance->m_notification();
            }
        }
    }
}

#if defined(CORE_CM4)
extern "C" void HSEM2_IRQHandler()
{
    profiling::interruptEnter();
    HAL_HSEM_IRQHandler();
    profiling::interruptExit();
}
#else
extern "C" void HSEM1_IRQHandler()
{
    profiling::interruptEnter();
    HAL_HSEM_IRQHandler();
    profiling::interruptExit();
}
#endif
#if defined(GIGA_MOTION_BENCH)
extern "C" void motion_bench_doorbell(std::uint32_t) noexcept;
#endif
extern "C" void HAL_HSEM_FreeCallback(std::uint32_t mask)
{
    hal::InterCoreChannel::dispatchNotification(mask);
#if defined(GIGA_MOTION_BENCH)
    motion_bench_doorbell(mask);
#endif
}

// NOLINTEND(clang-analyzer-core.FixedAddressDereference,cppcoreguidelines-pro-type-cstyle-cast,performance-no-int-to-ptr)
