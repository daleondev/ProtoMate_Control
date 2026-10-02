#include "hal/board/board.hpp"
#include "hal/drivers/factory/gpio.hpp"
#include "hal/hal.hpp"

#include "pneumo/pneumo.hpp"

#include <chrono>
#include <cstdio>
#include <thread>

int main()
{
    runtime::thread::publish_attributes({ .name = "Logging", .priority = 20, .stack_size = 8192UZ });
    pnm::log::initialize();

    pnm::log::info("Application started");

    // Keep drivers disabled; all three STEP timers are initialized but stopped.
    [[maybe_unused]] const auto steppers_enable_n{ hal::gpio::createOutput({
      .pin = { .port = hal::gpio::Port::F, .number = 3U },
      .initial_level = hal::gpio::Level::High,
      .type = hal::gpio::OutputType::PushPull,
      .pull = hal::gpio::Pull::None,
      .speed = hal::gpio::Speed::Low,
    }) };
    [[maybe_unused]] const auto m1_step{ hal::gpio::createOutput({
      .pin = { .port = hal::gpio::Port::E, .number = 9U },
      .initial_level = hal::gpio::Level::Low,
      .type = hal::gpio::OutputType::PushPull,
      .pull = hal::gpio::Pull::None,
      .speed = hal::gpio::Speed::Low,
      .alternate_function = 1U,
    }) };
    [[maybe_unused]] const auto m1_dir{ hal::gpio::createOutput({
      .pin = { .port = hal::gpio::Port::E, .number = 11U },
      .initial_level = hal::gpio::Level::Low,
      .type = hal::gpio::OutputType::PushPull,
      .pull = hal::gpio::Pull::None,
      .speed = hal::gpio::Speed::Low,
    }) };
    [[maybe_unused]] const auto m2_step{ hal::gpio::createOutput({
      .pin = { .port = hal::gpio::Port::C, .number = 6U },
      .initial_level = hal::gpio::Level::Low,
      .type = hal::gpio::OutputType::PushPull,
      .pull = hal::gpio::Pull::None,
      .speed = hal::gpio::Speed::Low,
      .alternate_function = 3U,
    }) };
    [[maybe_unused]] const auto m2_dir{ hal::gpio::createOutput({
      .pin = { .port = hal::gpio::Port::C, .number = 7U },
      .initial_level = hal::gpio::Level::Low,
      .type = hal::gpio::OutputType::PushPull,
      .pull = hal::gpio::Pull::None,
      .speed = hal::gpio::Speed::Low,
    }) };
    [[maybe_unused]] const auto m3_step{ hal::gpio::createOutput({
      .pin = { .port = hal::gpio::Port::D, .number = 14U },
      .initial_level = hal::gpio::Level::Low,
      .type = hal::gpio::OutputType::PushPull,
      .pull = hal::gpio::Pull::None,
      .speed = hal::gpio::Speed::Low,
      .alternate_function = 2U,
    }) };
    [[maybe_unused]] const auto m3_dir{ hal::gpio::createOutput({
      .pin = { .port = hal::gpio::Port::D, .number = 15U },
      .initial_level = hal::gpio::Level::Low,
      .type = hal::gpio::OutputType::PushPull,
      .pull = hal::gpio::Pull::None,
      .speed = hal::gpio::Speed::Low,
    }) };

    // Preserve TIM3's AF2 inputs; no encoder counting or index callback yet.
    [[maybe_unused]] const auto m1_encoder_a{ hal::gpio::createInput({
      .pin = { .port = hal::gpio::Port::B, .number = 4U },
      .pull = hal::gpio::Pull::None,
      .edge = hal::gpio::Edge::None,
      .alternate_function = 2U,
    }) };
    [[maybe_unused]] const auto m1_encoder_b{ hal::gpio::createInput({
      .pin = { .port = hal::gpio::Port::B, .number = 5U },
      .pull = hal::gpio::Pull::None,
      .edge = hal::gpio::Edge::None,
      .alternate_function = 2U,
    }) };
    [[maybe_unused]] const auto m1_encoder_z{ hal::gpio::createInput({
      .pin = { .port = hal::gpio::Port::B, .number = 6U },
      .pull = hal::gpio::Pull::None,
      .edge = hal::gpio::Edge::Rising,
    }) };

    const auto led{ hal::board::createLed(hal::board::LedId::Green) };
    if (!led) {
        hal::panic("Green LED creation failed");
    }

    auto t{ pnm::utils::concurrent::spawn_thread<std::thread>([] {
        const auto led{ hal::board::createLed(hal::board::LedId::Yellow) };
        if (!led) {
            hal::panic("Yellow LED creation failed");
        }
        while (true) {
            led->toggle();
            std::this_thread::sleep_for(std::chrono::milliseconds{ 100 });
        }
    }) };

    while (true) {
        led->toggle();
        std::this_thread::sleep_for(std::chrono::milliseconds{ 500 });
    }

    return 0;
}
