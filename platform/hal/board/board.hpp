#pragma once

#include "hal/devices/itf/IButton.hpp"
#include "hal/devices/itf/ILed.hpp"
#include "hal/devices/itf/IMotorFeedback.hpp"
#include "hal/devices/itf/IStepperDriver.hpp"
#include <array>
#include "hal/drivers/itf/IDigitalInput.hpp"
#include "hal/drivers/itf/IDigitalOutput.hpp"
#include "hal/drivers/itf/IQuadratureEncoder.hpp"
#include "hal/drivers/itf/IStepGenerator.hpp"
#include "hal/drivers/itf/IUart.hpp"
#include "hal/drivers/itf/ISpi.hpp"

#include <cstdint>
#include <memory>

namespace hal::board
{
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

    [[nodiscard]] auto createLed(LedId id) -> std::shared_ptr<device::ILed>;
    [[nodiscard]] auto createButton(ButtonId id) -> std::shared_ptr<device::IButton>;

    // LAN9255 ESC: SPI5 PF7/PF8/PF9, mode 0 at 937.5 kHz; CS PF6 active low.
    // Create CS first. Bus ownership is exclusive; no automatic SPI traffic.
    [[nodiscard]] auto createEthercatSpi() -> std::shared_ptr<ISpi>;
    [[nodiscard]] auto createEthercatChipSelect() -> std::shared_ptr<IDigitalOutput>;

    // Exclusive, uncached resources. Creation leaves outputs/counting stopped.
    [[nodiscard]] auto createStepperGenerator() -> std::shared_ptr<IStepGenerator>;
    [[nodiscard]] auto createStepperStepOutput(const std::shared_ptr<IStepGenerator>& generator, MotorId id)
      -> std::shared_ptr<IStepOutput>;
    // DIR starts low; its meaning as a physical direction belongs to motor control.
    [[nodiscard]] auto createStepperDirectionOutput(MotorId id) -> std::shared_ptr<IDigitalOutput>;
    // One shared active-low enable: high disables all three drivers at creation.
    [[nodiscard]] auto createSteppersEnableOutput() -> std::shared_ptr<IDigitalOutput>;
    // Only M1 currently has an encoder. Index never changes its count implicitly.
    [[nodiscard]] auto createEncoder(MotorId id) -> std::shared_ptr<IQuadratureEncoder>;
    // Counts at the motor shaft, after x4 decoding; zero means no encoder.
    [[nodiscard]] auto encoderCountsPerRevolution(MotorId id) noexcept -> std::uint32_t;
    [[nodiscard]] auto createEncoderIndex(MotorId id) -> std::shared_ptr<IDigitalInput>;
    // NC switch to ground: low = released, high = actuated or disconnected.
    // Pull-up, both edges, exclusive ownership; raw levels without debounce.
    [[nodiscard]] auto createReferenceLimitSwitch(MotorId id) -> std::shared_ptr<IDigitalInput>;
    [[nodiscard]] auto createStepperDriverBus() -> std::shared_ptr<IUart>;
    // M1 PF2/CN9.17: DM542T ALM+, pull-up, rising EXTI; ALM- to GND.
    // Default normally conducting ALM: low = healthy, high = fault/open cable.
    // M2 PD4/CN9.8, M3 PD3/CN9.10: active-high DIAG, pull-down, rising EXTI.
    [[nodiscard]] auto createStepperDiagnostic(MotorId id) -> std::shared_ptr<IDigitalInput>;
    // Normal electrical INDEX (not index_step): M2 PD0/CN9.25, M3 PD1/CN9.27.
    // 3.3 V, pull-down, both edges; four full steps per electrical cycle.
    [[nodiscard]] auto createStepperIndex(MotorId id) -> std::shared_ptr<IDigitalInput>;

    // Creates all driver endpoints over one shared, serialized UART connection.
    // No initialization or enable; M1 ALM and M2/M3 DIAG are monitored immediately.
    [[nodiscard]] auto createStepperDrivers(const std::array<std::size_t, 3>& microsteps,
                                            const std::shared_ptr<IDigitalOutput>& enable)
      -> std::array<std::shared_ptr<device::IStepperDriver>, 3>;

    struct MotorFeedbackConfig
    {
        pnm::units::Angle full_step_angle;
        std::size_t microsteps;
    };
    // M1: measured shaft angle via quadrature. M2/M3: driver electrical INDEX.
    // Owns the exclusive inputs; leaves observation stopped. Scaling comes from
    // motor configuration; encoder resolution and pin selection belong here.
    [[nodiscard]] auto createMotorFeedback(MotorId id, const MotorFeedbackConfig& config)
      -> std::shared_ptr<device::IMotorFeedback>;
}
