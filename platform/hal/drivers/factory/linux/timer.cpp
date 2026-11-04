#include "hal/drivers/factory/timer.hpp"

#include "hal/drivers/impl/linux/RemoteTimer.hpp"
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
        constexpr std::uint32_t TIMER_12_INPUT_FREQUENCY_HZ{ 64'000'000U };
        constexpr ITimer::Tick TIMER_12_PRESCALER{ 63U };
        constexpr ITimer::Tick TIMER_12_AUTO_RELOAD{ 65'535U };
    }

    auto create(Configuration configuration) -> std::shared_ptr<ITimer>
    {
        switch (configuration.peripheral) {
            case Peripheral::Tim5: {
                if (simulation::client::connected()) {
                    static auto remote{ std::make_shared<RemoteTimer>(5, 240000000, 239, UINT32_MAX) };
                    return remote;
                }
                static auto timer{ std::make_shared<Timer>(
                  Timer::Configuration{ .input_frequency_hz = TIMER_5_INPUT_FREQUENCY_HZ,
                                        .prescaler = TIMER_5_PRESCALER,
                                        .auto_reload = TIMER_5_AUTO_RELOAD }) };
                return timer;
            }
            case Peripheral::Tim12: {
                if (simulation::client::connected()) {
                    static auto remote{ std::make_shared<RemoteTimer>(12, 64000000, 63, 65535) };
                    return remote;
                }
                static auto timer{ std::make_shared<Timer>(
                  Timer::Configuration{ .input_frequency_hz = TIMER_12_INPUT_FREQUENCY_HZ,
                                        .prescaler = TIMER_12_PRESCALER,
                                        .auto_reload = TIMER_12_AUTO_RELOAD }) };
                return timer;
            }
            default:
                return {};
        }
    }
}
