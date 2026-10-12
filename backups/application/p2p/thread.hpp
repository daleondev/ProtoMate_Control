#pragma once

namespace pnm::coro
{
    class Context;
}

namespace p2p
{
    // Application-owned context driven only by system_threads::p2p().
    // Construct/register bridges against it before starting the system threads.
    // Each future core application gets its own instance.
    pnm::coro::Context& context();
}
