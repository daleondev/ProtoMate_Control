#pragma once

#include "hal/board/board.hpp"
#include "hal/util/Core.hpp"

#include <meta>

namespace hal::core
{
    // A missing factory means this role is unavailable. The catalog contains
    // construction recipes, never live devices or shared mutable state.
    struct Devices
    {
        std::shared_ptr<device::ILed> (*status_led)(){};
        std::shared_ptr<device::ILed> (*error_led)(){};
        std::shared_ptr<device::IButton> (*user_button)(){};
        std::shared_ptr<ITimer> (*runtime_timer)(){};
        std::shared_ptr<IRtc> (*rtc)(){};
        std::shared_ptr<IRng> (*rng)(){};
        std::shared_ptr<IInterCoreChannel> (*intercore_channel)(){};
        decltype(&board::create_stepper_generator) stepper_generator{};
        decltype(&board::create_stepper_step_output) stepper_step_output{};
        decltype(&board::create_stepper_direction_output) stepper_direction_output{};
        decltype(&board::create_steppers_enable_output) steppers_enable_output{};
        decltype(&board::create_reference_limit_switch) reference_limit_switch{};
        decltype(&board::create_stepper_driver_bus) stepper_driver_bus{};
        decltype(&board::create_stepper_drivers) stepper_drivers{};
        decltype(&board::create_motor_feedback) motor_feedback{};
    };

    // H755 processor assignments. Keep physical pins in board, and application
    // roles (IO/UI) outside HAL. Shared devices use synchronized implementations.
    struct Catalog
    {
        [[= util::Core::M4]] static constexpr Devices m4{
            .status_led = [] { return board::create_led(board::LedId::Green); },
            .runtime_timer = [] { return board::create_timer(timer::Peripheral::Tim5); },
            .rtc = board::create_rtc,
            .rng = board::create_rng,
            .intercore_channel = board::create_intercore_channel,
            .stepper_generator = board::create_stepper_generator,
            .stepper_step_output = board::create_stepper_step_output,
            .stepper_direction_output = board::create_stepper_direction_output,
            .steppers_enable_output = board::create_steppers_enable_output,
            .reference_limit_switch = board::create_reference_limit_switch,
            .stepper_driver_bus = board::create_stepper_driver_bus,
            .stepper_drivers = board::create_stepper_drivers,
            .motor_feedback = board::create_motor_feedback,
        };

        [[= util::Core::M7]] static constexpr Devices m7{
            .status_led = [] { return board::create_led(board::LedId::Yellow); },
            .error_led = [] { return board::create_led(board::LedId::Red); },
            .user_button = [] { return board::create_button(board::ButtonId::User); },
            .runtime_timer = [] { return board::create_timer(timer::Peripheral::Tim12); },
            .rtc = board::create_rtc,
            .rng = board::create_rng,
            .intercore_channel = board::create_intercore_channel,
        };

        [[= util::Core::Host]] static constexpr Devices host{
            .error_led = [] { return board::create_led(board::LedId::Red); },
            .user_button = [] { return board::create_button(board::ButtonId::User); },
            .runtime_timer = [] { return board::create_timer(timer::Peripheral::Tim5); },
            .rtc = board::create_rtc,
            .rng = board::create_rng,
            .stepper_generator = board::create_stepper_generator,
            .stepper_step_output = board::create_stepper_step_output,
            .stepper_direction_output = board::create_stepper_direction_output,
            .steppers_enable_output = board::create_steppers_enable_output,
            .reference_limit_switch = board::create_reference_limit_switch,
            .stepper_driver_bus = board::create_stepper_driver_bus,
            .stepper_drivers = board::create_stepper_drivers,
            .motor_feedback = board::create_motor_feedback,
        };
    };

    // Compile-time lookup only: callers cannot select a peer at runtime.
    [[nodiscard]] consteval auto devices_for(util::Core processor) -> Devices
    {
        Devices result{};
        bool found{};
        for (const auto member : std::meta::static_data_members_of(^^Catalog, std::meta::access_context::current())) {
            const auto owners = std::meta::annotations_of(member);
            if (owners.size() != 1) {
                throw "Each device catalog entry needs exactly one core annotation";
            }
            const auto owner = std::meta::extract<util::Core>(owners.front());
            if (owner != util::Core::M4 && owner != util::Core::M7 && owner != util::Core::Host) {
                throw "Unknown core in device catalog";
            }
            if (owner == processor) {
                if (found) { throw "Duplicate core in device catalog"; }
                result = std::meta::extract<Devices>(member);
                found = true;
            }
        }
        if (!found) { throw "No device catalog for this core"; }
        return result;
    }
}
