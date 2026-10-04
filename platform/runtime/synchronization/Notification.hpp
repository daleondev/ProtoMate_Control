#pragma once

#include <chrono>
#include <tx_api.h>

namespace runtime
{
    // One consumer, any number of producers. Signals coalesce and remain
    // pending until consumed. Construct/destroy/wait in thread context; signal
    // is allocation-free and safe from an ISR or an interrupt-masked callback.
    class Notification final
    {
      public:
        Notification();
        ~Notification();
        Notification(const Notification&) = delete;
        Notification& operator=(const Notification&) = delete;
        void signal() noexcept;
        // Consume one pending notification, or return false at the deadline.
        bool waitUntil(std::chrono::steady_clock::time_point deadline);
        void clear(); // Consumer only, before installing the next operation.

      private:
        TX_EVENT_FLAGS_GROUP m_event{};
    };
}
