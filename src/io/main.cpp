#include "Application.hpp"
#include "hal/core/core.hpp"
#include "hal/hal.hpp"

#include <memory>

int main()
{
    auto led = hal::core::create_status_led();
    if (!led) {
        hal::panic("IO status LED unavailable");
    }
    // Each firmware constructs only its own application, inside its own runtime.
    auto application = std::make_unique<io::Application>(std::move(led));
    application->start();
    application->join();
    return 0;
}
