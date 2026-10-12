#include "Application.hpp"
#include "hal/board/board.hpp"
#include "hal/hal.hpp"

#include <memory>

int main()
{
    auto led = hal::board::create_led(hal::board::LedId::Yellow);
    if (!led) {
        hal::panic("UI status LED unavailable");
    }
    // Each firmware constructs only its own application, inside its own runtime.
    auto application = std::make_unique<ui::Application>(std::move(led));
    application->start();
    application->join();
    return 0;
}
