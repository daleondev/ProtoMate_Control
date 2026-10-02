#include "Gpio.hpp"

#include "hal/stm32/InterruptGuard.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <utility>

namespace hal
{
    namespace
    {
        constexpr std::uint32_t GPIO_INTERRUPT_PRIORITY{ 5U };

        [[nodiscard]] auto gpio_port(gpio::Port port) noexcept -> GPIO_TypeDef*
        {
            switch (port) {
                using enum gpio::Port;
                case A:
                    return GPIOA;
                case B:
                    return GPIOB;
                case C:
                    return GPIOC;
                case D:
                    return GPIOD;
                case E:
                    return GPIOE;
                case F:
                    return GPIOF;
                case G:
                    return GPIOG;
                case H:
                    return GPIOH;
                case I:
                    return GPIOI;
                case J:
                    return GPIOJ;
                case K:
                    return GPIOK;
            }
            return nullptr;
        }

        auto enable_port_clock(gpio::Port port) noexcept -> void
        {
            switch (port) {
                using enum gpio::Port;
                case A:
                    __HAL_RCC_GPIOA_CLK_ENABLE();
                    break;
                case B:
                    __HAL_RCC_GPIOB_CLK_ENABLE();
                    break;
                case C:
                    __HAL_RCC_GPIOC_CLK_ENABLE();
                    break;
                case D:
                    __HAL_RCC_GPIOD_CLK_ENABLE();
                    break;
                case E:
                    __HAL_RCC_GPIOE_CLK_ENABLE();
                    break;
                case F:
                    __HAL_RCC_GPIOF_CLK_ENABLE();
                    break;
                case G:
                    __HAL_RCC_GPIOG_CLK_ENABLE();
                    break;
                case H:
                    __HAL_RCC_GPIOH_CLK_ENABLE();
                    break;
                case I:
                    __HAL_RCC_GPIOI_CLK_ENABLE();
                    break;
                case J:
                    __HAL_RCC_GPIOJ_CLK_ENABLE();
                    break;
                case K:
                    __HAL_RCC_GPIOK_CLK_ENABLE();
                    break;
            }
        }

        [[nodiscard]] constexpr auto pin_mask(std::uint8_t number) noexcept -> std::uint16_t
        {
            return static_cast<std::uint16_t>(1U << number);
        }

        [[nodiscard]] auto interrupt_for(std::uint8_t number) noexcept -> IRQn_Type
        {
            switch (number) {
                case 0U:
                    return EXTI0_IRQn;
                case 1U:
                    return EXTI1_IRQn;
                case 2U:
                    return EXTI2_IRQn;
                case 3U:
                    return EXTI3_IRQn;
                case 4U:
                    return EXTI4_IRQn;
                case 5U:
                case 6U:
                case 7U:
                case 8U:
                case 9U:
                    return EXTI9_5_IRQn;
                default:
                    return EXTI15_10_IRQn;
            }
        }

        [[nodiscard]] auto hal_pull(gpio::Pull pull) noexcept -> std::uint32_t
        {
            switch (pull) {
                using enum gpio::Pull;
                case Up:
                    return GPIO_PULLUP;
                case Down:
                    return GPIO_PULLDOWN;
                case None:
                    return GPIO_NOPULL;
            }
            return GPIO_NOPULL;
        }

        [[nodiscard]] auto input_mode(gpio::Edge edge) noexcept -> std::uint32_t
        {
            switch (edge) {
                using enum gpio::Edge;
                case Rising:
                    return GPIO_MODE_IT_RISING;
                case Falling:
                    return GPIO_MODE_IT_FALLING;
                case Both:
                    return GPIO_MODE_IT_RISING_FALLING;
                case None:
                    return GPIO_MODE_INPUT;
            }
            return GPIO_MODE_INPUT;
        }

        [[nodiscard]] auto output_mode(gpio::OutputType type) noexcept -> std::uint32_t
        {
            return type == gpio::OutputType::OpenDrain ? GPIO_MODE_OUTPUT_OD : GPIO_MODE_OUTPUT_PP;
        }

        [[nodiscard]] auto output_speed(gpio::Speed speed) noexcept -> std::uint32_t
        {
            switch (speed) {
                using enum gpio::Speed;
                case Low:
                    return GPIO_SPEED_FREQ_LOW;
                case Medium:
                    return GPIO_SPEED_FREQ_MEDIUM;
                case High:
                    return GPIO_SPEED_FREQ_HIGH;
                case VeryHigh:
                    return GPIO_SPEED_FREQ_VERY_HIGH;
            }
            return GPIO_SPEED_FREQ_LOW;
        }

        [[nodiscard]] auto hal_level(gpio::Level level) noexcept -> GPIO_PinState
        {
            return level == gpio::Level::High ? GPIO_PIN_SET : GPIO_PIN_RESET;
        }

        [[nodiscard]] auto gpio_level(GPIO_PinState state) noexcept -> gpio::Level
        {
            return state == GPIO_PIN_SET ? gpio::Level::High : gpio::Level::Low;
        }
    }

    GpioInput::GpioInput(gpio::InputConfiguration configuration)
      : m_configuration{ configuration }
      , m_port{ gpio_port(configuration.pin.port) }
      , m_pinMask{ pin_mask(configuration.pin.number) }
      , m_interrupt{ interrupt_for(configuration.pin.number) }
    {
        enable_port_clock(configuration.pin.port);

        if (configuration.edge != gpio::Edge::None) {
            HAL_NVIC_DisableIRQ(m_interrupt);
            if (!interruptLineAvailable(configuration.pin)) {
                std::terminate();
            }
            s_interruptRegistry[configuration.pin.number] = this;
        }

        GPIO_InitTypeDef gpio_configuration{};
        gpio_configuration.Pin = m_pinMask;
        gpio_configuration.Mode = input_mode(configuration.edge);
        gpio_configuration.Pull = hal_pull(configuration.pull);
        gpio_configuration.Speed = GPIO_SPEED_FREQ_LOW;
        HAL_GPIO_Init(m_port, &gpio_configuration);

        if (configuration.edge != gpio::Edge::None) {
            // NVIC pending state is shared by grouped EXTI lines. Clear only
            // this pin's peripheral flag so a previous owner cannot trigger
            // the new callback, while preserving other pins' pending edges.
            __HAL_GPIO_EXTI_CLEAR_IT(m_pinMask);
            HAL_NVIC_SetPriority(m_interrupt, GPIO_INTERRUPT_PRIORITY, 0U);
            HAL_NVIC_EnableIRQ(m_interrupt);
        }
    }

    GpioInput::~GpioInput()
    {
        if (m_configuration.edge != gpio::Edge::None) {
            HAL_NVIC_DisableIRQ(m_interrupt);
            HAL_GPIO_DeInit(m_port, m_pinMask);
            __HAL_GPIO_EXTI_CLEAR_IT(m_pinMask);
            s_interruptRegistry[m_configuration.pin.number] = nullptr;

            const bool group_still_used{ std::ranges::any_of(s_interruptRegistry, [&](const auto* input) {
                return input != nullptr && input->m_interrupt == m_interrupt;
            }) };
            if (group_still_used) {
                HAL_NVIC_EnableIRQ(m_interrupt);
            }
            else {
                HAL_NVIC_ClearPendingIRQ(m_interrupt);
            }
        }
        else {
            HAL_GPIO_DeInit(m_port, m_pinMask);
        }
    }

    auto GpioInput::read() const noexcept -> gpio::Level
    {
        return gpio_level(HAL_GPIO_ReadPin(m_port, m_pinMask));
    }

    auto GpioInput::setEdgeCallback(EdgeCallback callback) noexcept -> void
    {
        {
            // Exclude both EXTI and a task switch between concurrent setters.
            // Destroy the previous callback after leaving the critical section.
            const stm32::InterruptGuard interrupt_guard;
            m_edgeCallback.swap(callback);
        }
    }

    auto GpioInput::interruptLineAvailable(gpio::Pin pin) noexcept -> bool
    {
        return pin.number < s_interruptRegistry.size() && s_interruptRegistry[pin.number] == nullptr;
    }

    auto GpioInput::dispatchEdge(std::uint16_t pin_mask_value) noexcept -> void
    {
        for (std::size_t number{}; number < s_interruptRegistry.size(); ++number) {
            auto* const input{ s_interruptRegistry[number] };
            if (input != nullptr && (pin_mask_value & pin_mask(static_cast<std::uint8_t>(number))) != 0U &&
                input->m_edgeCallback) {
                input->m_edgeCallback(input->read());
            }
        }
    }

    GpioOutput::GpioOutput(gpio::OutputConfiguration configuration)
      : m_port{ gpio_port(configuration.pin.port) }
      , m_pinMask{ pin_mask(configuration.pin.number) }
    {
        enable_port_clock(configuration.pin.port);
        HAL_GPIO_WritePin(m_port, m_pinMask, hal_level(configuration.initial_level));

        GPIO_InitTypeDef gpio_configuration{};
        gpio_configuration.Pin = m_pinMask;
        gpio_configuration.Mode = output_mode(configuration.type);
        gpio_configuration.Pull = hal_pull(configuration.pull);
        gpio_configuration.Speed = output_speed(configuration.speed);
        HAL_GPIO_Init(m_port, &gpio_configuration);
    }

    GpioOutput::~GpioOutput() { HAL_GPIO_DeInit(m_port, m_pinMask); }

    auto GpioOutput::read() const noexcept -> gpio::Level
    {
        return gpio_level(HAL_GPIO_ReadPin(m_port, m_pinMask));
    }

    auto GpioOutput::write(gpio::Level level) noexcept -> void
    {
        HAL_GPIO_WritePin(m_port, m_pinMask, hal_level(level));
    }

    auto GpioOutput::toggle() noexcept -> void
    {
        const stm32::InterruptGuard interrupt_guard;
        // Toggle the output latch, not IDR: an open-drain output can be held
        // low externally even when its latch is high (released).
        HAL_GPIO_TogglePin(m_port, m_pinMask);
    }
}

extern "C" void HAL_GPIO_EXTI_Callback(std::uint16_t pin_mask) { hal::GpioInput::dispatchEdge(pin_mask); }

extern "C" void EXTI0_IRQHandler() { HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_0); }
extern "C" void EXTI1_IRQHandler() { HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_1); }
extern "C" void EXTI2_IRQHandler() { HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_2); }
extern "C" void EXTI3_IRQHandler() { HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_3); }
extern "C" void EXTI4_IRQHandler() { HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_4); }

extern "C" void EXTI9_5_IRQHandler()
{
    HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_5);
    HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_6);
    HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_7);
    HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_8);
    HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_9);
}

extern "C" void EXTI15_10_IRQHandler()
{
    HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_10);
    HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_11);
    HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_12);
    HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_13);
    HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_14);
    HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_15);
}
