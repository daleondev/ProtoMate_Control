#include "Application.hpp"
#include "hal/core/core.hpp"
#include "hal/hal.hpp"
#include "p2p/transport/InterCoreTransport.hpp"

#include <memory>

int main()
{
    auto led = hal::core::create_status_led();
    if (!led) {
        hal::panic("IO status LED unavailable");
    }
    auto rng = hal::core::create_rng();
    if (!rng || !rng->generate()) { hal::panic("Hardware entropy unavailable"); }
    // Each firmware constructs only its own application, inside its own runtime.
    p2p::InterCoreTransport transport{hal::core::create_intercore_channel()};
    auto application = std::make_unique<io::Application>(transport, std::move(led));
    application->start();
    application->join();
    return 0;
}
