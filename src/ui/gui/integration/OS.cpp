#include "OS.hpp"
#include "runtime/synchronization/Notification.hpp"
#include <array>
#include <exception>
#include <memory>
#include <stdexcept>
#include <thread>
#include <touchgfx/hal/OSWrappers.hpp>
#include <tx_api.h>
namespace
{
    struct Synchronization
    {
        Synchronization(const Synchronization&) = delete;
        Synchronization& operator=(const Synchronization&) = delete;
        Synchronization(Synchronization&&) = delete;
        Synchronization& operator=(Synchronization&&) = delete;
        TX_SEMAPHORE framebuffer{};
        runtime::Notification vsync;
        std::stop_token stop;
        explicit Synchronization(std::stop_token token)
          : stop{ std::move(token) }
        {
            static auto name{ std::to_array("TouchGFX framebuffer") };
            if (tx_semaphore_create(&framebuffer, name.data(), 1) != TX_SUCCESS) {
                std::terminate();
            }
        }
        ~Synchronization() { static_cast<void>(tx_semaphore_delete(&framebuffer)); }
    };
    // TouchGFX OS callback ABI has no context argument; Session owns this single binding.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    std::unique_ptr<Synchronization> synchronization{};
}
namespace ui::gui::os
{
    Session::Session(std::stop_token stop)
    {
        if (synchronization) {
            throw std::logic_error("TouchGFX synchronization already owned");
        }
        synchronization = std::make_unique<Synchronization>(std::move(stop));
    }
    Session::~Session() { synchronization.reset(); }
}
namespace touchgfx
{
    namespace
    {
        constexpr auto STOP_POLL_INTERVAL{ std::chrono::milliseconds{ 20 } };
        constexpr ULONG FRAMEBUFFER_WAIT_TICKS{ 20 };
    }
    void OSWrappers::initialize() {}
    void OSWrappers::deinitialize() {}
    void OSWrappers::signalVSync() { synchronization->vsync.signal(); }
    void OSWrappers::signalRenderingDone() { synchronization->vsync.clear(); }
    void OSWrappers::waitForVSync()
    {
        while (!synchronization->stop.stop_requested() &&
               !synchronization->vsync.waitUntil(std::chrono::steady_clock::now() + STOP_POLL_INTERVAL)) {
        }
    }
    bool OSWrappers::isVSyncAvailable()
    {
        return synchronization->vsync.waitUntil(std::chrono::steady_clock::now());
    }
    void OSWrappers::takeFrameBufferSemaphore()
    {
        while (!synchronization->stop.stop_requested() &&
               tx_semaphore_get(&synchronization->framebuffer, FRAMEBUFFER_WAIT_TICKS) != TX_SUCCESS) {
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
    void simulator_printf(const char* /*unused*/, va_list /*unused*/) {}
#endif
}
#if defined(HAL_PLATFORM_STM32)
#include <touchgfx/hal/GPIO.hpp>
namespace touchgfx
{
    // No instrumentation pins are reserved; timings are recorded by Controller.
    void GPIO::init() {}
    void GPIO::set(GPIO_ID /*unused*/) {}
    void GPIO::clear(GPIO_ID /*unused*/) {}
    void GPIO::toggle(GPIO_ID /*unused*/) {}
    bool GPIO::get(GPIO_ID /*unused*/) { return false; }
}
#endif
