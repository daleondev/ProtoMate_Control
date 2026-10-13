#include "hal/drivers/factory/rgb.hpp"
#if defined(CORE_CM7)
#include "hal/drivers/impl/stm32/RgbOutput.hpp"
#include "hal/drivers/factory/gpio.hpp"
#include "hal/drivers/util/ExclusiveInstances.hpp"
#include "ltdc.h"
#include "dma2d.h"
#include "tim.h"
#endif
namespace hal::rgb {
    auto create(Configuration configuration) -> std::shared_ptr<IRgbOutput>
    {
#if defined(CORE_CM7)
        if (configuration.peripheral != Peripheral::Ltdc || configuration.backlight != gpio::Pin{gpio::Port::C,6}) { return {}; }
        auto pin = gpio::create_output({.pin=configuration.backlight, .alternate_function=3});
        if (!pin) { return {}; }
        static util::ExclusiveInstances<1> instances;
        return instances.create<RgbOutput>(0, hltdc, hdma2d, htim8, std::move(pin));
#else
        static_cast<void>(configuration); return {};
#endif
    }
}
