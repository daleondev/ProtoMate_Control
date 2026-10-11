#pragma once

#include "../transport/ITransport.hpp"

#include <atomic>

namespace p2p::util
{
    // Readiness callbacks may finish after unregistration. They retain only
    // this shared signal, never the bridge or its helpers. At most one wakeup
    // is queued; the sender clears pending and rescans after consuming it.
    struct OutgoingSignal
    {
        pnm::coro::Channel<bool> ready;
        std::atomic_flag pending;

        void notify() noexcept
        {
            if (pending.test_and_set())
                return;
            try {
                ready.push(true);
            } catch (...) {
                // Wake the sender to report failure rather than wait forever.
                ready.close();
            }
        }
    };

    // Independent waiters prevent one sender from consuming the other's wakeup.
    // A readiness event may affect both lanes (for example a timed-out request).
    struct OutgoingWakeup
    {
        OutgoingSignal normal, urgent;
        OutgoingSignal& signal(Lane lane) { return lane == Lane::Urgent ? urgent : normal; }
        void notify() noexcept
        {
            urgent.notify();
            normal.notify();
        }
        void close()
        {
            urgent.ready.close();
            normal.ready.close();
        }
    };
}
