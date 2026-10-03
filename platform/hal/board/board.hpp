#pragma once

#include "hal/devices/itf/IButton.hpp"
#include "hal/devices/itf/ILed.hpp"
#include "hal/drivers/itf/IDigitalInput.hpp"
#include "hal/drivers/itf/IPwmOutput.hpp"
#include "hal/drivers/itf/IQuadratureEncoder.hpp"

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
        M1,
        M2,
        M3
    };

    [[nodiscard]] auto createLed(LedId id) -> std::shared_ptr<device::ILed>;
    [[nodiscard]] auto createButton(ButtonId id) -> std::shared_ptr<device::IButton>;

    // Exclusive, uncached resources. Creation leaves outputs/counting stopped.
    [[nodiscard]] auto createStepperStepOutput(MotorId id) -> std::shared_ptr<IPwmOutput>;
    // Only M1 currently has an encoder. Index never changes its count implicitly.
    [[nodiscard]] auto createEncoder(MotorId id) -> std::shared_ptr<IQuadratureEncoder>;
    [[nodiscard]] auto createEncoderIndex(MotorId id) -> std::shared_ptr<IDigitalInput>;
}
