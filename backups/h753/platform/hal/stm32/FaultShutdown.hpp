#pragma once

namespace hal::stm32
{
    // Terminal shutdown only: leaves interrupts masked and bypasses all HAL
    // objects, callbacks and RTOS services. Safe before peripheral setup.
    auto shutdown_motion_on_fault() noexcept -> void;
}
