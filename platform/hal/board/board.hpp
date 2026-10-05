#pragma once

#include "hal/devices/itf/IButton.hpp"
#include "hal/devices/itf/ILed.hpp"
#include "hal/drivers/itf/IDigitalInput.hpp"
#include "hal/drivers/itf/IDigitalOutput.hpp"
#include "hal/drivers/itf/IQuadratureEncoder.hpp"
#include "hal/drivers/itf/IStepGenerator.hpp"
#include "hal/drivers/itf/IUart.hpp"

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
    // M2 PD4/CN9.8, M3 PD3/CN9.10: active-high DIAG, pull-down, rising EXTI.
    [[nodiscard]] auto createStepperDiagnostic(MotorId id) -> std::shared_ptr<IDigitalInput>;
}
