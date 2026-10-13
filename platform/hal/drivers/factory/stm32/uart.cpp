#include "hal/drivers/factory/uart.hpp"
#include "hal/drivers/factory/gpio.hpp"
#include "hal/drivers/impl/stm32/Uart.hpp"
#include "hal/drivers/util/ExclusiveInstances.hpp"
#include "hal/drivers/util/UartTransfer.hpp"

#include <utility>

namespace hal::uart
{
    auto create(Configuration configuration) -> std::shared_ptr<IUart>
    {
        if (configuration.peripheral != Peripheral::Usart2 ||
            (configuration.tx != gpio::Pin{gpio::Port::D, 5} || configuration.rx != gpio::Pin{gpio::Port::D, 6}) ||
            !util::valid_uart_configuration(configuration.transport) ||
            !__HAL_RCC_USART2_IS_CLK_ENABLED()) {
            return {};
        }
        constexpr auto enabled{ USART_CR1_UE | USART_CR1_TE | USART_CR1_RE | USART_CR1_FIFOEN };
        if ((USART2->CR1 & (enabled | USART_CR1_M0 | USART_CR1_M1 | USART_CR1_PCE)) != enabled ||
            (USART2->CR2 & USART_CR2_STOP) != 0U) {
            return {};
        }
        auto tx{ gpio::create_output({ .pin = configuration.tx,
                                      .initial_level = gpio::Level::High,
                                      .pull = gpio::Pull::Up,
                                      .alternate_function = 7U }) };
        if (!tx) { return {}; }
        auto rx{ gpio::create_input({ .pin = configuration.rx,
                                     .pull = gpio::Pull::Up,
                                     .alternate_function = 7U }) };
        if (!rx) { return {}; }

        // Startup configures the peripheral once. A private handle describes
        // that hardware for blocking HAL transfers; no generated handle is
        // shared between processors and construction sends no bytes.
        static UART_HandleTypeDef handle = [] {
            UART_HandleTypeDef result{};
            result.Instance = USART2;
            result.Init.WordLength = UART_WORDLENGTH_8B;
            result.Init.Parity = UART_PARITY_NONE;
            result.gState = HAL_UART_STATE_READY;
            result.RxState = HAL_UART_STATE_READY;
            result.Mask = 0xFFU;
            result.FifoMode = UART_FIFOMODE_ENABLE;
            return result;
        }();
        static util::ExclusiveInstances<1> instances;
        return instances.create<Uart>(0U, Uart::HardwareConfiguration{
            .handle = handle, .configuration = configuration.transport,
            .tx = std::move(tx), .rx = std::move(rx) });
    }
}
