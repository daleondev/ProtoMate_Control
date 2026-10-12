#include "Gpio.hpp"

#include <exception>
#include <mutex>
#include <utility>

namespace hal
{
    GpioInput::GpioInput(gpio::InputConfiguration configuration)
      : m_configuration{ configuration }
      , m_level{ configuration.pull == gpio::Pull::Up ? gpio::Level::High : gpio::Level::Low }
    {
    }

    GpioInput::~GpioInput() = default;

    auto GpioInput::read() const noexcept -> gpio::Level { return m_level.load(std::memory_order_acquire); }

    auto GpioInput::setEdgeCallback(EdgeCallback callback) noexcept -> void
    {
        std::shared_ptr<EdgeCallback> replacement;
        if (callback) {
            try {
                replacement = std::make_shared<EdgeCallback>(std::move(callback));
            } catch (...) {
                std::terminate();
            }
        }

        const std::scoped_lock lock{ m_callbackMutex };
        m_edgeCallback = std::move(replacement);
    }

    auto GpioInput::setSimulatedLevel(gpio::Level level) noexcept -> void
    {
        const gpio::Level previous{ m_level.exchange(level, std::memory_order_acq_rel) };
        if (!edgeMatches(previous, level)) {
            return;
        }

        std::shared_ptr<EdgeCallback> callback;
        {
            const std::scoped_lock lock{ m_callbackMutex };
            callback = m_edgeCallback;
        }
        if (callback && *callback) {
            (*callback)(level);
        }
    }

    auto GpioInput::edgeMatches(gpio::Level previous, gpio::Level current) const noexcept -> bool
    {
        if (previous == current) {
            return false;
        }
        switch (m_configuration.edge) {
            using enum gpio::Edge;
            case Rising:
                return current == gpio::Level::High;
            case Falling:
                return current == gpio::Level::Low;
            case Both:
                return true;
            case None:
                return false;
        }
        return false;
    }

    GpioOutput::GpioOutput(gpio::OutputConfiguration configuration) noexcept
      : m_level{ configuration.initial_level }
    {
    }

    auto GpioOutput::read() const noexcept -> gpio::Level { return m_level.load(std::memory_order_acquire); }

    auto GpioOutput::write(gpio::Level level) noexcept -> void
    {
        m_level.store(level, std::memory_order_release);
    }

    auto GpioOutput::toggle() noexcept -> void
    {
        gpio::Level expected{ m_level.load(std::memory_order_relaxed) };
        while (!m_level.compare_exchange_weak(
          expected, gpio::inverted(expected), std::memory_order_acq_rel, std::memory_order_relaxed)) {
        }
    }
}
