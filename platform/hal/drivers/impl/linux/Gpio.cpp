#include "Gpio.hpp"

#include <exception>
#include <utility>

namespace hal
{
    namespace
    {
        class PthreadLock
        {
          public:
            explicit PthreadLock(pthread_mutex_t& mutex) noexcept
              : m_mutex{ mutex }
            {
                if (pthread_mutex_lock(&m_mutex) != 0) {
                    std::terminate();
                }
            }

            ~PthreadLock()
            {
                if (pthread_mutex_unlock(&m_mutex) != 0) {
                    std::terminate();
                }
            }

            PthreadLock(const PthreadLock&) = delete;
            PthreadLock& operator=(const PthreadLock&) = delete;
            PthreadLock(PthreadLock&&) = delete;
            PthreadLock& operator=(PthreadLock&&) = delete;

          private:
            pthread_mutex_t& m_mutex;
        };
    }

    GpioInput::GpioInput(gpio::InputConfiguration configuration)
      : m_configuration{ configuration }
      , m_level{ configuration.pull == gpio::Pull::Up ? gpio::Level::High : gpio::Level::Low }
    {
        if (pthread_mutex_init(&m_callbackMutex, nullptr) != 0) {
            std::terminate();
        }
    }

    GpioInput::~GpioInput()
    {
        if (pthread_mutex_destroy(&m_callbackMutex) != 0) {
            std::terminate();
        }
    }

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

        PthreadLock lock{ m_callbackMutex };
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
            PthreadLock lock{ m_callbackMutex };
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
