#include "hal/drivers/factory/rgb.hpp"
namespace hal::rgb {
    auto create(Configuration) -> std::shared_ptr<IRgbOutput> { return {}; }
}
