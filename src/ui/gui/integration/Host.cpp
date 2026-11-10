#include "hal/linux/DisplayHost.hpp"
#include <ctime>
#include <tx_api.h>
extern "C" void hmi_native_observe(void*, const hal::display::State&, std::uint32_t);
extern "C" void hmi_native_close();
// Native SDL cleanup precedes process-wide C++ destruction. The latter must
// execute on the ThreadX application thread because our runtime owns std::mutex.
extern "C" int runtime_host_main(void (*kernel)())
{
    const auto status{ hal::linux::display::run(kernel, hmi_native_observe) };
    hmi_native_close();
    if (!hal::linux::display::cleanup_complete()) {
        return status;
    }
    for (;;) {
        const timespec delay{ .tv_sec = 0, .tv_nsec = 1000000 };
        nanosleep(&delay, nullptr);
    }
}
extern "C" void runtime_host_completed(int status)
{
    hal::linux::display::completed(status);
    while (!hal::linux::display::cleanup_complete()) {
        tx_thread_sleep(1);
    }
}
