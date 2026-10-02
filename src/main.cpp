#include "hal/board/board.hpp"
#include "hal/hal.hpp"

#include <chrono>
#include <cstdio>
#include <thread>

int main()
{
    if (std::puts("[template] ready") < 0 || std::fflush(stdout) != 0) {
        hal::panic("Template console initialization failed");
    }
    const auto led{ hal::board::createLed(hal::board::LedId::Green) };
    if (!led) {
        hal::panic("Green LED creation failed");
    }
    while (true) {
        led->toggle();
        std::this_thread::sleep_for(std::chrono::milliseconds{ 500 });
    }
}
