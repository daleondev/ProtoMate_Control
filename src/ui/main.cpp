#include "Application.hpp"
#include "hal/core/core.hpp"
#include "icc/transport/InterCoreTransport.hpp"

#include <memory>

int main()
{
    // Each firmware constructs only its own application, inside its own runtime.
    icc::InterCoreTransport transport{ hal::core::create_intercore_channel() };
    auto application = std::make_unique<ui::Application>(transport);
    application->start();
    application->join();
    return 0;
}
