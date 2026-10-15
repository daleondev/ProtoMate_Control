#include "hal/linux/DisplayHost.hpp"
#include <ctime>
#include <tx_api.h>
// Native SDL cleanup precedes process-wide C++ destruction. The latter must
// execute on the ThreadX application thread because our runtime owns std::mutex.
extern "C" int runtime_host_main(void (*kernel)())
{
    const auto status = hal::linux::display::run(kernel);
    if (!hal::linux::display::cleanupComplete()) {
        return status;
    }
    for (;;) {
        const timespec delay{ 0, 1000000 };
        nanosleep(&delay, nullptr);
    }
}
extern "C" void runtime_host_completed(int status)
{
    hal::linux::display::completed(status);
    while (!hal::linux::display::cleanupComplete()) {
        tx_thread_sleep(1);
    }
}
