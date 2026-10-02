#include "Led.hpp"

#include <exception>
#include <utility>

namespace hal::device
{
    Led::Led(std::shared_ptr<IDigitalOutput> output, gpio::Level active_level)
      : m_output{ std::move(output) }
      , m_activeLevel{ active_level }
    {
        if (m_output == nullptr) {
            std::terminate();
        }
    }

    auto Led::turnOn() noexcept -> void { m_output->write(m_activeLevel); }

    auto Led::turnOff() noexcept -> void { m_output->write(gpio::inverted(m_activeLevel)); }

    auto Led::toggle() noexcept -> void { m_output->toggle(); }

    auto Led::isOn() const noexcept -> bool { return m_output->read() == m_activeLevel; }
}
