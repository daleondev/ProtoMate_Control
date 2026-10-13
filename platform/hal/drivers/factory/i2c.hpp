#pragma once
#include "hal/drivers/itf/II2c.hpp"
#include <memory>
namespace hal::i2c { auto create(Peripheral peripheral) -> std::shared_ptr<II2c>; }
