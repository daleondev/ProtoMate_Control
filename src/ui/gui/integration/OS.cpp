#include "OS.hpp"
#include "runtime/synchronization/Notification.hpp"
#include <exception>
#include <memory>
#include <thread>
#include <touchgfx/hal/OSWrappers.hpp>
#include <tx_api.h>
namespace
{
    struct Synchronization
    {
        TX_SEMAPHORE framebuffer{};
        runtime::Notification vsync;
        std::stop_token stop;
        explicit Synchronization(std::stop_token token)
          : stop(token)
        {
            static char name[]{ "TouchGFX framebuffer" };
            if (tx_semaphore_create(&framebuffer, name, 1) != TX_SUCCESS) {
                std::terminate();
            }
        }
        ~Synchronization() { static_cast<void>(tx_semaphore_delete(&framebuffer)); }
    };
    std::unique_ptr<Synchronization> synchronization;
}
namespace ui::gui::os
{
    void prepare(std::stop_token stop) { synchronization = std::make_unique<Synchronization>(stop); }
    void finish() { synchronization.reset(); }
}
namespace touchgfx
{
    void OSWrappers::initialize() {}
    void OSWrappers::deinitialize() {}
    void OSWrappers::signalVSync() { synchronization->vsync.signal(); }
    void OSWrappers::signalRenderingDone() { synchronization->vsync.clear(); }
    void OSWrappers::waitForVSync()
    {
        while (!synchronization->stop.stop_requested() &&
               !synchronization->vsync.waitUntil(std::chrono::steady_clock::now() +
                                                 std::chrono::milliseconds{ 20 })) {
        }
    }
    bool OSWrappers::isVSyncAvailable()
    {
        return synchronization->vsync.waitUntil(std::chrono::steady_clock::now());
    }
    void OSWrappers::takeFrameBufferSemaphore()
    {
        while (!synchronization->stop.stop_requested() &&
               tx_semaphore_get(&synchronization->framebuffer, 20) != TX_SUCCESS) {
        }
    }
    void OSWrappers::tryTakeFrameBufferSemaphore()
    {
        static_cast<void>(tx_semaphore_get(&synchronization->framebuffer, TX_NO_WAIT));
    }
    void OSWrappers::giveFrameBufferSemaphore()
    {
        static_cast<void>(tx_semaphore_ceiling_put(&synchronization->framebuffer, 1));
    }
    void OSWrappers::giveFrameBufferSemaphoreFromISR() { giveFrameBufferSemaphore(); }
    void OSWrappers::taskDelay(uint16_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds{ ms }); }
    void OSWrappers::taskYield() { std::this_thread::yield(); }
#if defined(HAL_PLATFORM_LINUX)
    // The engine's printf hook is diagnostic only, never its SDL simulator.
    void simulator_printf(const char*, va_list) {}
#endif
}
#if defined(HAL_PLATFORM_STM32)
#include <touchgfx/hal/GPIO.hpp>
namespace touchgfx
{
    // No instrumentation pins are reserved; timings are recorded by Controller.
    void GPIO::init() {}
    void GPIO::set(GPIO_ID) {}
    void GPIO::clear(GPIO_ID) {}
    void GPIO::toggle(GPIO_ID) {}
    bool GPIO::get(GPIO_ID) { return false; }
}
#endif
