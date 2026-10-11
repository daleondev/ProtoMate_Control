#pragma once

#include <pneumo/coroutines.hpp>

#include <atomic>

namespace p2p::util
{
    // Readiness callbacks may finish after unregistration. They retain only
    // this shared signal, never the bridge or its helpers. At most one wakeup
    // is queued; the sender clears pending and rescans after consuming it.
    struct OutgoingWakeup
    {
        pnm::coro::Channel<bool> ready;
        std::atomic_flag pending{};

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
}
