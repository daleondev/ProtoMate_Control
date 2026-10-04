#include "hal/drivers/factory/gpio.hpp"
#include "hal/linux/Mutex.hpp"

#include "hal/drivers/impl/linux/Gpio.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <mutex>
#include <type_traits>

namespace hal::gpio
{
    namespace
    {
        constexpr std::size_t PINS_PER_PORT{ 16U };
        constexpr std::size_t PORT_COUNT{ 11U };
        constexpr std::size_t PIN_COUNT{ PINS_PER_PORT * PORT_COUNT };

        struct PinOwner
        {
            std::weak_ptr<void> instance;
            std::weak_ptr<GpioInput> input;
        };

        std::array<PinOwner, PIN_COUNT> owners;
        linux::Mutex owners_mutex;

        [[nodiscard]] auto pin_index(Pin pin) noexcept -> std::size_t
        {
            return static_cast<std::size_t>(pin.port) * PINS_PER_PORT + pin.number;
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

            const std::scoped_lock lock{ owners_mutex };
            auto& owner{ owners[pin_index(configuration.pin)] };
            if (!owner.instance.expired()) {
                return {};
            }
            auto instance{ std::make_shared<Implementation>(configuration) };
            owner.instance = instance;
            if constexpr (std::is_same_v<Implementation, GpioInput>) {
                owner.input = instance;
            } else {
                owner.input.reset();
            }
            return instance;
        }
    }

    auto simulatedInput(Pin pin) -> std::shared_ptr<GpioInput>
    {
        if (!valid(pin)) {
            return {};
        }
        const std::scoped_lock lock{ owners_mutex };
        return owners[pin_index(pin)].input.lock();
    }

    auto createInput(InputConfiguration configuration) -> std::shared_ptr<IDigitalInput>
    {
        if (configuration.alternate_function &&
            (*configuration.alternate_function > 15U || configuration.edge != Edge::None)) {
            return {};
        }
        return create_pin<IDigitalInput, GpioInput>(configuration);
    }

    auto createOutput(OutputConfiguration configuration) -> std::shared_ptr<IDigitalOutput>
    {
        if (configuration.alternate_function && *configuration.alternate_function > 15U) {
            return {};
        }
        return create_pin<IDigitalOutput, GpioOutput>(configuration);
    }
}
