#include "core.hpp"

#include "hal/board/board.hpp"

namespace hal::core
{
    auto create_status_led() -> std::shared_ptr<device::ILed>
    {
        return board::create_status_led(id());
    }

    auto create_runtime_timer() -> std::shared_ptr<ITimer>
    {
        return board::create_runtime_timer(id());
    }
}
