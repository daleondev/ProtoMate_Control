#include "hal/drivers/factory/timer.hpp"

#include "hal/drivers/impl/linux/Timer.hpp"

#include <cstdint>
#include <limits>
#include <memory>

namespace hal::timer
{
    namespace
    {
        constexpr std::size_t TIMER_2_INDEX{ 2U };
        constexpr std::uint32_t TIMER_2_INPUT_FREQUENCY_HZ{ 240'000'000U };
        constexpr ITimer::Tick TIMER_2_PRESCALER{ 239U };
        constexpr ITimer::Tick TIMER_2_AUTO_RELOAD{ std::numeric_limits<ITimer::Tick>::max() };
    }

    auto create(std::size_t index) -> std::shared_ptr<ITimer>
    {
        if (index != TIMER_2_INDEX) {
            return {};
        }

        static auto timer{ std::make_shared<Timer>(Timer::Configuration{
          .input_frequency_hz = TIMER_2_INPUT_FREQUENCY_HZ,
          .prescaler = TIMER_2_PRESCALER,
          .auto_reload = TIMER_2_AUTO_RELOAD }) };
        return timer;
    }
}
