#include "app/Application.hpp"
#include "common/icc/transport/InterCoreTransport.hpp"
#include "hal/core/core.hpp"

#include <memory>

int main()
{
    // Each firmware constructs only its own application, inside its own runtime.
    icc::InterCoreTransport transport{ hal::core::create_intercore_channel() };
    auto application = std::make_unique<io::Application>(transport);
    // This firmware has its own logger backend. On Linux, IO and UI share a
    // process, so motion receives the IO sink explicitly instead.
    application->installLogSink();
    application->start();
    application->join();
    return 0;
}
