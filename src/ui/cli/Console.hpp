#pragma once

#include <stop_token>

namespace ui::cli
{
    class Parser;

    // Selects ncurses/plain input on Linux, and the serial console on STM32.
    // Returns on EOF, Ctrl-C/Ctrl-D, or application shutdown.
    void run(Parser& parser, std::stop_token stop);

    // Plain console frontend shared by redirected Linux input and STM32 UART.
    // Input waits yield through the runtime so ICC can continue independently.
    void runStream(Parser& parser, std::stop_token stop, bool echo);
}
