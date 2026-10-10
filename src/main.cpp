#include "hal/board/board.hpp"
#include "hal/drivers/factory/ethernet.hpp"
#include "hal/hal.hpp"

#include "cli/Parser.hpp"
#include "cli/axis.hpp"
#include "cli/motor.hpp"
#include "cli/robot.hpp"
#include "control/ControllerServer.hpp"
#include "control/MotionController.hpp"
#include "control/Robot.hpp"
#include "p2p/InProcessTransport.hpp"
#include "pneumo/pneumo.hpp"
#include "system_threads.hpp"

#if defined(HAL_PLATFORM_LINUX)
#include "cli/linux/Console.hpp"
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <thread>
#include <utility>

namespace
{
    using namespace pnm::units::literals;
    constexpr std::size_t LOG_STACK_SIZE{ 8192U };
    constexpr auto DUMMY_Z_TRAVEL{ 40_mm };
    struct Contexts
    {
        pnm::coro::Context io;
        pnm::coro::Context ui;
    };
    Contexts& contexts()
    {
        // Construct after the C++ runtime starts; lifetime covers all system threads.
        static Contexts value;
        return value;
    }
}

auto system_threads::control() -> void { contexts().io.run(); }
auto system_threads::messages() -> void { contexts().ui.run(); }

auto system_threads::led() -> void
{
    const auto green_led{ hal::board::create_led(hal::board::LedId::Green) };
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

    const auto user_button{ hal::board::create_button(hal::board::ButtonId::User) };
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
try {
    runtime::thread::publish_attributes(
      { .name = "Logging", .priority = system_threads::PRIO._20, .stack_size = LOG_STACK_SIZE });
    pnm::log::initialize();
#if defined(HAL_PLATFORM_LINUX)
    cli::terminal::configure_logging();
#endif

    // Compose hardware once on the IO side. Robot and CLI share the UI client;
    // every control request crosses the serialized execution boundary.
    const auto motion{ std::make_shared<control::MotionController>(
      std::array<control::MotionController::AxisConfig, 3>{ {
        { .reference_position = 135_deg, .full_step_angle = 1.8_deg, .microsteps = 16U },
        { .reference_position = 135_deg, .full_step_angle = 1.8_deg, .microsteps = 16U },
        { .reference_position = 135_deg, .full_step_angle = 1.8_deg, .microsteps = 16U },
      } }) };

    // DUMMY MECHANICS: replace these values with the robot's actual reductions,
    // coordinate directions and switch coordinates when they are available.
    using enum hal::board::MotorId;
    motion->configureAxis(Motor1,
                          RotaryAxisConversion{ {
                            .motor_revolutions_per_axis_revolution = 1.0,
                            .direction = AxisDirection::SameAsMotor,
                            .motor_reference = motion->motorConfiguration(Motor1).reference_position,
                            .axis_reference = 0_deg,
                          } });
    motion->configureAxis(Motor2,
                          RotaryAxisConversion{ {
                            .motor_revolutions_per_axis_revolution = 1.0,
                            .direction = AxisDirection::SameAsMotor,
                            .motor_reference = motion->motorConfiguration(Motor2).reference_position,
                            .axis_reference = 0_deg,
                          } });
    motion->configureAxis(Motor3,
                          LinearAxisConversion{ {
                            .travel_per_output_revolution = DUMMY_Z_TRAVEL,
                            .motor_revolutions_per_output_revolution = 1.0,
                            .direction = AxisDirection::SameAsMotor,
                            .motor_reference = motion->motorConfiguration(Motor3).reference_position,
                            .axis_reference = 0_mm,
                          } });
    auto& context{ contexts() };
    pnm::msg::Bus io_bus;
    pnm::msg::Bus ui_bus;
    auto transport = p2p::InProcessTransport::pair();
    control::ControllerServer server(motion, io_bus, context.io);
    p2p::Bridge io_bridge(io_bus, context.io, transport[0]);
    p2p::Bridge ui_bridge(ui_bus, context.ui, transport[1]);
    control::protocol::routes(io_bridge, true);
    control::protocol::routes(ui_bridge, false);
    io_bridge.onStateChanged([&server](auto state) { server.linkChanged(state); });
    auto client = std::make_shared<control::ControllerClient>(ui_bus, context.ui);
    io_bridge.start();
    ui_bridge.start();
    // Nominal CAD parameter lengths. Base/tool calibration and travel limits
    // remain placeholders, like the mechanics above; this enables calculations.
    const auto robot{ std::make_shared<control::Robot>(
      client,
      ScaraKinematics::Config{ .first_arm_length = 205_mm,
                               .second_arm_length = 223.4_mm,
                               .tool_offset = { .x = 135_mm, .y = 0_mm, .z = 0_mm },
                               .limits = {} }) };
    cli::motor::setup(cli::registry(), client);
    cli::axis::setup(cli::registry(), client);
    cli::robot::setup(cli::registry(), robot);
    system_threads::start();

    while (true) {
        std::this_thread::sleep_for(1h);
    }

    std::unreachable();
} catch (const std::exception& error) {
    std::fputs("Application startup failed: ", stderr);
    std::fputs(error.what(), stderr);
    std::fputc('\n', stderr);
    return EXIT_FAILURE;
} catch (...) {
    std::fputs("Application startup failed with an unknown exception\n", stderr);
    return EXIT_FAILURE;
}
