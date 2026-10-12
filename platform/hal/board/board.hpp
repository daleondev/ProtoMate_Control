#pragma once

#include "hal/devices/itf/IButton.hpp"
#include "hal/devices/itf/ILed.hpp"
#include "hal/devices/itf/IMotorFeedback.hpp"
#include "hal/devices/itf/IStepperDriver.hpp"
#include "hal/drivers/itf/IDigitalInput.hpp"
#include "hal/drivers/itf/IDigitalOutput.hpp"
#include "hal/drivers/itf/IQuadratureEncoder.hpp"
#include "hal/drivers/itf/ISpi.hpp"
#include "hal/drivers/itf/ITimer.hpp"
#include "hal/drivers/itf/IStepGenerator.hpp"
#include "hal/drivers/itf/IUart.hpp"
#include "hal/drivers/itf/IRtc.hpp"
#include "hal/drivers/itf/IRng.hpp"
#include "hal/drivers/itf/IInterCoreChannel.hpp"
#include <array>

#include <cstdint>
#include <memory>

namespace hal::board
{
    // Physical wiring and device construction only. Access is independent of
    // the caller; processor assignments are defined separately in hal::core.
    enum class LedId : std::uint8_t
    {
        Green,
        Yellow,
        Red
    };

    enum class ButtonId : std::uint8_t
    {
        User
    };

    enum class MotorId : std::uint8_t
    {
        Motor1,
        Motor2,
        Motor3
    };

    [[nodiscard]] auto create_led(LedId id) -> std::shared_ptr<device::ILed>;
    [[nodiscard]] auto create_button(ButtonId id) -> std::shared_ptr<device::IButton>;
    [[nodiscard]] auto create_timer(timer::Peripheral peripheral) -> std::shared_ptr<ITimer>;
    [[nodiscard]] auto create_rtc() -> std::shared_ptr<IRtc>;
    [[nodiscard]] auto create_rng() -> std::shared_ptr<IRng>;
    [[nodiscard]] auto create_intercore_channel() -> std::shared_ptr<IInterCoreChannel>;

    // LAN9255 ESC bus and active-low chip select.
    // Create CS first. Bus ownership is exclusive; no automatic SPI traffic.
    [[nodiscard]] auto create_ethercat_spi() -> std::shared_ptr<ISpi>;
    [[nodiscard]] auto create_ethercat_chip_select() -> std::shared_ptr<IDigitalOutput>;

    // Exclusive, uncached resources. Creation leaves outputs/counting stopped.
    [[nodiscard]] auto create_stepper_generator() -> std::shared_ptr<IStepGenerator>;
    [[nodiscard]] auto create_stepper_step_output(const std::shared_ptr<IStepGenerator>& generator,
                                                  MotorId id) -> std::shared_ptr<IStepOutput>;
    // DIR starts low; its meaning as a physical direction belongs to motor control.
    [[nodiscard]] auto create_stepper_direction_output(MotorId id) -> std::shared_ptr<IDigitalOutput>;
    // One shared active-low enable: high disables all three drivers at creation.
    [[nodiscard]] auto create_steppers_enable_output() -> std::shared_ptr<IDigitalOutput>;
    // Only M1 currently has an encoder. Index never changes its count implicitly.
    [[nodiscard]] auto create_encoder(MotorId id) -> std::shared_ptr<IQuadratureEncoder>;
    // Counts at the motor shaft, after x4 decoding; zero means no encoder.
    [[nodiscard]] auto encoder_counts_per_revolution(MotorId id) noexcept -> std::uint32_t;
    [[nodiscard]] auto create_encoder_index(MotorId id) -> std::shared_ptr<IDigitalInput>;
    // NC switch to ground: low = released, high = actuated or disconnected.
    // Pull-up, both edges, exclusive ownership; raw levels without debounce.
    [[nodiscard]] auto create_reference_limit_switch(MotorId id) -> std::shared_ptr<IDigitalInput>;
    [[nodiscard]] auto create_stepper_driver_bus() -> std::shared_ptr<IUart>;
    // M1: DM542T ALM+, pull-up, rising EXTI; ALM- to GND.
    // Default normally conducting ALM: low = healthy, high = fault/open cable.
    // M2/M3: active-high DIAG, pull-down, rising EXTI.
    [[nodiscard]] auto create_stepper_diagnostic(MotorId id) -> std::shared_ptr<IDigitalInput>;
    // Normal electrical INDEX (not index_step) for M2/M3.
    // 3.3 V, pull-down, both edges; four full steps per electrical cycle.
    [[nodiscard]] auto create_stepper_index(MotorId id) -> std::shared_ptr<IDigitalInput>;

    // Creates all driver endpoints over one shared, serialized UART connection.
    // No initialization or enable; M1 ALM and M2/M3 DIAG are monitored immediately.
    // An injected transport replaces the board bus; empty selects its default.
    [[nodiscard]] auto create_stepper_drivers(const std::array<std::size_t, 3>& microsteps,
                                              const std::shared_ptr<IDigitalOutput>& enable,
                                              std::shared_ptr<IUart> uart = {})
      -> std::array<std::shared_ptr<device::IStepperDriver>, 3>;

    struct MotorFeedbackConfig
    {
        pnm::units::Angle full_step_angle;
        std::size_t microsteps{};
    };
    // M1: measured shaft angle via quadrature. M2/M3: driver electrical INDEX.
    // Owns the exclusive inputs; leaves observation stopped. Scaling comes from
    // motor configuration; encoder resolution and pin selection belong here.
    [[nodiscard]] auto create_motor_feedback(MotorId id, const MotorFeedbackConfig& config)
      -> std::shared_ptr<device::IMotorFeedback>;
}
