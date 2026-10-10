#pragma once

#include "hal/drivers/itf/IDigitalInput.hpp"
#include "hal/drivers/itf/IDigitalOutput.hpp"

#include <memory>

namespace hal::gpio
{
    [[nodiscard]] auto create_input(InputConfiguration configuration) -> std::shared_ptr<IDigitalInput>;
    [[nodiscard]] auto create_output(OutputConfiguration configuration) -> std::shared_ptr<IDigitalOutput>;
}
