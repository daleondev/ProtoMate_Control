#include "hal/board/board.hpp"
#include "hal/hal.hpp"

#include "pneumo/pneumo.hpp"

#include <chrono>
#include <cstdio>
#include <thread>

int main()
{
    runtime::thread::publish_attributes({ .name = "Logging", .priority = 20, .stack_size = 8192UZ });
    pnm::log::initialize();

    pnm::log::info("Application started");

    const auto led{ hal::board::createLed(hal::board::LedId::Green) };
    if (!led) {
        hal::panic("Green LED creation failed");
    }

    auto t{ pnm::utils::concurrent::spawn_thread<std::thread>([] {
        const auto led{ hal::board::createLed(hal::board::LedId::Yellow) };
        if (!led) {
            hal::panic("Yellow LED creation failed");
        }
        while (true) {
            led->toggle();
            std::this_thread::sleep_for(std::chrono::milliseconds{ 100 });
        }
    }) };

    while (true) {
        led->toggle();
        std::this_thread::sleep_for(std::chrono::milliseconds{ 500 });
    }

    return 0;
}
