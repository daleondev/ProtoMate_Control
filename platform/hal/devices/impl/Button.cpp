#include "Button.hpp"

#include <exception>
#include <utility>

namespace hal::device
{
    Button::Button(std::shared_ptr<IDigitalInput> input, gpio::Level active_level)
      : m_input{ std::move(input) }
      , m_activeLevel{ active_level }
    {
        if (m_input == nullptr) {
            std::terminate();
        }
    }

    Button::~Button() { m_input->clearEdgeCallback(); }

    auto Button::state() const noexcept -> State { return stateFromLevel(m_input->read()); }

    auto Button::setStateChangedCallback(StateChangedCallback callback) noexcept -> void
    {
        if (!callback) {
            m_input->clearEdgeCallback();
            return;
        }

        m_input->setEdgeCallback(
          [active_level = m_activeLevel, callback = std::move(callback)](gpio::Level level) mutable noexcept {
            callback(level == active_level ? State::Pressed : State::Released);
        });
    }

    auto Button::stateFromLevel(gpio::Level level) const noexcept -> State
    {
        return level == m_activeLevel ? State::Pressed : State::Released;
    }
}
