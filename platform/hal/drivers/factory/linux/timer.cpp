#include "hal/drivers/factory/timer.hpp"

#include "hal/drivers/impl/linux/Timer.hpp"

#include <cstdint>
#include <limits>
#include <memory>

namespace hal::timer
{
    namespace
    {
        constexpr std::uint32_t TIMER_5_INPUT_FREQUENCY_HZ{ 240'000'000U };
        constexpr ITimer::Tick TIMER_5_PRESCALER{ 239U };
        constexpr ITimer::Tick TIMER_5_AUTO_RELOAD{ std::numeric_limits<ITimer::Tick>::max() };
    }

    auto create(Configuration configuration) -> std::shared_ptr<ITimer>
    {
        if (configuration.peripheral != Peripheral::Tim5) {
            return {};
        }

        static auto timer{ std::make_shared<Timer>(Timer::Configuration{
          .input_frequency_hz = TIMER_5_INPUT_FREQUENCY_HZ,
          .prescaler = TIMER_5_PRESCALER,
          .auto_reload = TIMER_5_AUTO_RELOAD }) };
        return timer;
    }
}
