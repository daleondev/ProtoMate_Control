#include "hal/drivers/factory/i2c.hpp"
namespace hal::i2c { auto create(Peripheral) -> std::shared_ptr<II2c> { return {}; } }
