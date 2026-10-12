#include "hal/drivers/factory/gpio.hpp"

#include "hal/drivers/impl/stm32/Gpio.hpp"

#include <tx_api.h>

#include <array>
#include <cstddef>
#include <exception>
#include <memory>
#include <type_traits>

namespace
{
    inline constexpr auto MAX_ALTERNATE_FUNCTION{ 15U };
}

namespace hal::gpio
{
    namespace
    {
        constexpr std::size_t PINS_PER_PORT{ 16U };
        constexpr std::size_t PORT_COUNT{ 11U };
        constexpr std::size_t PIN_COUNT{ PINS_PER_PORT * PORT_COUNT };

        struct PinOwner
        {
            bool claimed{};
        };

        auto owners() -> std::array<PinOwner, PIN_COUNT>&
        {
            static std::array<PinOwner, PIN_COUNT> value;
            return value;
        }

        class FactoryMutex
        {
          public:
            FactoryMutex(FactoryMutex&&) = delete;
            auto operator=(FactoryMutex&&) -> FactoryMutex& = delete;

            FactoryMutex()
            {
                static auto name{ std::to_array("GPIO factory") };
                if (tx_mutex_create(&m_mutex, name.data(), TX_INHERIT) != TX_SUCCESS) {
                    std::terminate();
                }
            }

            ~FactoryMutex()
            {
                if (tx_mutex_delete(&m_mutex) != TX_SUCCESS) {
                    std::terminate();
                }
            }

            FactoryMutex(const FactoryMutex&) = delete;
            FactoryMutex& operator=(const FactoryMutex&) = delete;

            auto lock() noexcept -> void
            {
                if (tx_mutex_get(&m_mutex, TX_WAIT_FOREVER) != TX_SUCCESS) {
                    std::terminate();
                }
            }

            auto unlock() noexcept -> void
            {
                if (tx_mutex_put(&m_mutex) != TX_SUCCESS) {
                    std::terminate();
                }
            }

          private:
            TX_MUTEX m_mutex{};
        };

        [[nodiscard]] auto owners_mutex() -> FactoryMutex&
        {
            static FactoryMutex mutex;
            return mutex;
        }

        class FactoryLock
        {
          public:
            FactoryLock(FactoryLock&&) = delete;
            auto operator=(FactoryLock&&) -> FactoryLock& = delete;

            explicit FactoryLock(FactoryMutex& mutex) noexcept
              : m_mutex{ mutex }
            {
                m_mutex.lock();
            }

            ~FactoryLock() { m_mutex.unlock(); }

            FactoryLock(const FactoryLock&) = delete;
            FactoryLock& operator=(const FactoryLock&) = delete;

          private:
            FactoryMutex& m_mutex;
        };

        [[nodiscard]] auto pin_index(Pin pin) noexcept -> std::size_t
        {
            return (static_cast<std::size_t>(pin.port) * PINS_PER_PORT) + pin.number;
        }

        [[nodiscard]] auto valid(Pin pin) noexcept -> bool
        {
            return static_cast<std::size_t>(pin.port) < PORT_COUNT && pin.number < PINS_PER_PORT;
        }

        template<typename Interface, typename Implementation, typename Configuration>
        [[nodiscard]] auto create_pin(Configuration configuration) -> std::shared_ptr<Interface>
        {
            if (!valid(configuration.pin)) {
                return {};
            }

            const FactoryLock lock{ owners_mutex() };
            const std::size_t index{ pin_index(configuration.pin) };
            auto& owner{ owners()[index] };
            if (owner.claimed) {
                return {};
            }
            if constexpr (std::is_same_v<Implementation, GpioInput>) {
                if (configuration.edge != Edge::None &&
                    !GpioInput::interruptLineAvailable(configuration.pin)) {
                    return {};
                }
            }
            auto instance{ std::shared_ptr<Implementation>{ new Implementation{ configuration },
                                                            [index](Implementation* implementation) noexcept {
                const FactoryLock lock{ owners_mutex() };
                // Destroy under the factory lock before releasing the pin claim.
                {
                    const std::unique_ptr<Implementation> released{ implementation };
                }
                owners()[index].claimed = false;
            } } };
            owner.claimed = true;
            return instance;
        }
    }

    auto create_input(InputConfiguration configuration) -> std::shared_ptr<IDigitalInput>
    {
        if (configuration.alternate_function && (*configuration.alternate_function > MAX_ALTERNATE_FUNCTION ||
                                                 configuration.edge != Edge::None)) {
            return {};
        }
        return create_pin<IDigitalInput, GpioInput>(configuration);
    }

    auto create_output(OutputConfiguration configuration) -> std::shared_ptr<IDigitalOutput>
    {
        if (configuration.alternate_function && *configuration.alternate_function > MAX_ALTERNATE_FUNCTION) {
            return {};
        }
        return create_pin<IDigitalOutput, GpioOutput>(configuration);
    }
}
