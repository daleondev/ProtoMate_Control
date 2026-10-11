#pragma once

#include <pneumo/coroutines.hpp>
#include <stop_token>
#include <thread>

#if defined(HAL_PLATFORM_LINUX) || defined(HAL_PLATFORM_STM32)
#include "runtime/thread.hpp"
#endif

namespace example
{
    // Example/test composition, not part of the portable bridge. The runtime's
    // default 4 KiB standard-thread stack is too small for Debug serialization.
    inline std::jthread runContext(pnm::coro::Context& context)
    {
        auto run = [&context](std::stop_token stop) {
            std::stop_callback cancellation{stop, [&context] { context.stop(); }};
            context.run();
        };
#if defined(HAL_PLATFORM_LINUX) || defined(HAL_PLATFORM_STM32)
        return runtime::thread::create_jthread({.name = "p2p", .stack_size = 32768}, run);
#else
        return std::jthread(run);
#endif
    }
}
