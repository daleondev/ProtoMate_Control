#include "hal/board/board.hpp"
#include "hal/drivers/factory/ethernet.hpp"
#include "hal/hal.hpp"

#include "pneumo/pneumo.hpp"
#include "system_threads.hpp"

#if defined(HAL_PLATFORM_LINUX)
#include "cli/linux/Console.hpp"
#endif

#include <chrono>
#include <print>
#include <thread>
#include <utility>

namespace
{
    using namespace pnm::units::literals;
}

auto system_threads::led() -> void
{
    const auto green_led{ hal::board::createLed(hal::board::LedId::Green) };
    if (!green_led) {
        throw(std::runtime_error("green LED creation failed"));
    }

    while (true) {
        green_led->toggle();
        std::this_thread::sleep_for(500ms);
    }

    std::unreachable();
}

auto system_threads::button() -> void
{
    static_assert(std::atomic_bool::is_always_lock_free);
    std::atomic_bool user_button_press_pending{};

    const auto user_button{ hal::board::createButton(hal::board::ButtonId::User) };
    if (!user_button) {
        throw(std::runtime_error("user button creation failed"));
    }
    user_button->setStateChangedCallback(
      [&user_button_press_pending](hal::device::IButton::State state) noexcept {
        if (state == hal::device::IButton::State::Pressed) {
            user_button_press_pending.store(true, std::memory_order_release);
        }
    });

    while (true) {
        if (user_button_press_pending.exchange(false, std::memory_order_acq_rel)) {
            pnm::log::info("user button pressed");
        }
        std::this_thread::sleep_for(100ms);
    }

    std::unreachable();
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char** argv)
{
    runtime::thread::publish_attributes(
      { .name = "Logging", .priority = system_threads::PRIO._20, .stack_size = 8192UZ });
    pnm::log::initialize();
#if defined(HAL_PLATFORM_LINUX)
    cli::terminal::configureLogging();
#endif

    system_threads::start();

    while (true) {
        std::this_thread::sleep_for(1h);
    }

    std::unreachable();
}
