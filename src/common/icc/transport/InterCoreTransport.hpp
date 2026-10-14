#pragma once

#include "ITransport.hpp"
#include "hal/drivers/itf/IInterCoreChannel.hpp"
#include "runtime/synchronization/Notification.hpp"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace icc
{
    // Adapter between the portable bridge and a physical core's byte channel.
    // ISR -> local RTOS notification -> worker -> coroutine wakeup. All waiters,
    // mutexes and task frames live in this core's private RAM.
    class InterCoreTransport final : public ITransport
    {
      public:
        explicit InterCoreTransport(std::shared_ptr<hal::IInterCoreChannel> channel);
        ~InterCoreTransport() override;
        pnm::coro::Task<pnm::EmptyResult<Error>> send(std::span<const std::byte> frame,
                                                      Lane lane = Lane::Normal,
                                                      std::stop_token stop = {}) override;
        pnm::coro::Task<pnm::Result<std::size_t, Error>> receive(std::span<std::byte> buffer,
                                                                 Lane lane = Lane::Normal,
                                                                 std::stop_token stop = {}) override;
        void close() noexcept;

      private:
        void notificationThread();
        void wakeWaiters();
        std::shared_ptr<hal::IInterCoreChannel> m_channel;
        runtime::Notification m_notification;
        std::mutex m_mutex;
        struct Waiter
        {
            pnm::coro::Channel<bool> ready;
            bool waiting{};
            bool pending{};
        };
        // Reused across operations. pending coalesces notifications even when a
        // cancellation leaves a wakeup queued for the next operation.
        std::array<Waiter, 2 * LANE_COUNT> m_waiters;
        bool m_closed{};
        std::atomic_bool m_stopping{};
        std::thread m_worker;
    };
}
