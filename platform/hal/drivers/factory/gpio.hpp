#pragma once

#include "hal/drivers/itf/IDigitalInput.hpp"
#include "hal/drivers/itf/IDigitalOutput.hpp"

#include <memory>

namespace hal::gpio
{
    [[nodiscard]] auto createInput(InputConfiguration configuration) -> std::shared_ptr<IDigitalInput>;
    [[nodiscard]] auto createOutput(OutputConfiguration configuration) -> std::shared_ptr<IDigitalOutput>;
}
