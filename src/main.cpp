#include "hal/board/board.hpp"
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
    const auto steppers_enable_n{ hal::board::createSteppersEnableOutput() };
    const auto m1_step{ hal::board::createStepperStepOutput(hal::board::MotorId::M1) };
    const auto m1_dir{ hal::board::createStepperDirectionOutput(hal::board::MotorId::M1) };
    const auto m2_step{ hal::board::createStepperStepOutput(hal::board::MotorId::M2) };
    const auto m2_dir{ hal::board::createStepperDirectionOutput(hal::board::MotorId::M2) };
    const auto m3_step{ hal::board::createStepperStepOutput(hal::board::MotorId::M3) };
    const auto m3_dir{ hal::board::createStepperDirectionOutput(hal::board::MotorId::M3) };

    const auto m1_encoder{ hal::board::createEncoder(hal::board::MotorId::M1) };
    const auto m1_encoder_z{ hal::board::createEncoderIndex(hal::board::MotorId::M1) };
    if (!steppers_enable_n || !m1_step || !m1_dir || !m2_step || !m2_dir || !m3_step || !m3_dir ||
        !m1_encoder || !m1_encoder_z) {
        hal::panic("Motor interface creation failed");
    }

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
