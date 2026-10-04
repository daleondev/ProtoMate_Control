#pragma once

#include "hal/devices/itf/IButton.hpp"
#include "hal/devices/itf/ILed.hpp"
#include "hal/drivers/itf/IDigitalInput.hpp"
#include "hal/drivers/itf/IDigitalOutput.hpp"
#include "hal/drivers/itf/IQuadratureEncoder.hpp"
#include "hal/drivers/itf/IStepGenerator.hpp"

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
    [[nodiscard]] auto createEncoderIndex(MotorId id) -> std::shared_ptr<IDigitalInput>;
    // NC switch to ground: low = released, high = actuated or disconnected.
    // Pull-up, both edges, exclusive ownership; raw levels without debounce.
    [[nodiscard]] auto createReferenceLimitSwitch(MotorId id) -> std::shared_ptr<IDigitalInput>;
}
