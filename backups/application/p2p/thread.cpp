#include "thread.hpp"
#include "../system_threads.hpp"

#include <pneumo/coroutines.hpp>

pnm::coro::Context& p2p::context()
{
    // Lazy construction avoids initializing synchronization primitives before
    // the runtime starts. Lifetime covers the bridges and their runner thread.
    static pnm::coro::Context value;
    return value;
}

auto system_threads::p2p() -> void
{
    // run() sleeps when idle and resumes the bridge coroutines when notified.
    ::p2p::context().run();
}
