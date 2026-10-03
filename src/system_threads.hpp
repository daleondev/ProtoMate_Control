#pragma once

#include "pneumo/pneumo.hpp"

#include <thread>

namespace system_threads
{
    using namespace pnm::units::literals;
    static constexpr auto PRIO{ pnm::meta::structural::range<TX_MAX_PRIORITIES, uint16_t>() };
#if defined(HAL_PLATFORM_LINUX)
    // ncurses and the Linux ThreadX port's signal/context-switch frames need
    // more host stack than the serial STM32 frontend.
    static constexpr auto CLI_STACK_SIZE{ 64_KB };
#else
    // FileX directory traversal has a large debug-build stack frame, and the
    // stream-aware parser keeps additional iostream state alive while a
    // command runs. Leave enough headroom for nested filesystem commands.
    static constexpr auto CLI_STACK_SIZE{ 16_KB };
#endif

    auto start() -> void;

    // ----------------------
    // Thread definitions
    // ----------------------

    [[ = PRIO._5, = CLI_STACK_SIZE ]] auto cli() -> void;

    [[ = PRIO._10, = 4_KB ]] auto led() -> void;

    [[ = PRIO._11, = 4_KB ]] auto button() -> void;
}
