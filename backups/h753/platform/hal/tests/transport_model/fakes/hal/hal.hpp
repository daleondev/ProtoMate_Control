#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>
#include <vector>

enum HAL_StatusTypeDef
{
    HAL_OK,
    HAL_ERROR,
    HAL_BUSY,
    HAL_TIMEOUT
};
inline constexpr unsigned SPI_CFG1_MBR_Pos = 28;
inline constexpr unsigned SPI_DATASIZE_8BIT = 7, SPI_DIRECTION_2LINES = 0;
inline constexpr unsigned UART_WORDLENGTH_9B = 9, UART_PARITY_NONE = 0;
struct SPI_HandleTypeDef
{
    struct
    {
        std::uint32_t BaudRatePrescaler{};
        std::uint32_t DataSize{ SPI_DATASIZE_8BIT }, Direction{ SPI_DIRECTION_2LINES };
    } Init{};
};
struct USART_TypeDef
{
    std::uint32_t ISR{};
};
struct UART_HandleTypeDef
{
    USART_TypeDef* Instance{};
    unsigned FifoMode{};
    struct
    {
        unsigned WordLength{ 8 }, Parity{ UART_PARITY_NONE };
    } Init{};
};
inline constexpr unsigned UART_FIFOMODE_ENABLE = 1;
inline constexpr unsigned USART_ISR_ORE = 8, USART_ISR_NE = 4, USART_ISR_FE = 2, USART_ISR_PE = 1;
inline constexpr unsigned UART_RXDATA_FLUSH_REQUEST = 0;
inline constexpr unsigned UART_CLEAR_OREF = 8, UART_CLEAR_NEF = 4, UART_CLEAR_FEF = 2, UART_CLEAR_PEF = 1;

namespace fake
{
    inline std::uint32_t ipsr{}, tick{};
    inline unsigned spi_init{}, spi_deinit{}, spi_reset{}, spi_transfers{}, uart_init{}, uart_deinit{};
    inline SPI_HandleTypeDef* spi_handle{};
    inline UART_HandleTypeDef* uart_handle{};
    inline HAL_StatusTypeDef spi_result{ HAL_OK }, tx_result{ HAL_OK };
    inline std::uint32_t spi_timeout{};
    inline std::vector<std::uint32_t> uart_timeouts;
    inline std::vector<std::uint8_t> transmitted, receive_after_transmit;
    inline std::deque<std::uint8_t> incoming;
    inline auto reset() -> void
    {
        ipsr = tick = 0;
        spi_init = spi_deinit = spi_reset = spi_transfers = uart_init = uart_deinit = 0;
        spi_handle = nullptr;
        uart_handle = nullptr;
        spi_result = tx_result = HAL_OK;
        uart_timeouts.clear();
        transmitted.clear();
        receive_after_transmit.clear();
        incoming.clear();
    }
}
inline auto __get_IPSR() { return fake::ipsr; }
inline auto HAL_GetTick() { return fake::tick; }
inline auto HAL_Delay(std::uint32_t ms) { fake::tick += ms; }
inline auto HAL_SPI_DeInit(SPI_HandleTypeDef* handle)
{
    ++fake::spi_deinit;
    fake::spi_handle = handle;
    return HAL_OK;
}
inline auto HAL_SPI_TransmitReceive(SPI_HandleTypeDef* handle,
                                    const std::uint8_t* tx,
                                    std::uint8_t* rx,
                                    std::uint16_t size,
                                    std::uint32_t timeout)
{
    ++fake::spi_transfers;
    fake::spi_handle = handle;
    fake::spi_timeout = timeout;
    if (fake::spi_result == HAL_OK)
        std::copy_n(tx, size, rx);
    return fake::spi_result;
}
inline auto HAL_UART_DeInit(UART_HandleTypeDef* handle)
{
    ++fake::uart_deinit;
    fake::uart_handle = handle;
    return HAL_OK;
}
inline auto HAL_UART_Transmit(UART_HandleTypeDef* handle,
                              const std::uint8_t* tx,
                              std::uint16_t size,
                              std::uint32_t timeout)
{
    fake::uart_handle = handle;
    fake::uart_timeouts.push_back(timeout);
    fake::transmitted.assign(tx, tx + size);
    fake::incoming.assign(fake::receive_after_transmit.begin(), fake::receive_after_transmit.end());
    fake::tick += 2;
    return fake::tx_result;
}
inline auto HAL_UART_Receive(UART_HandleTypeDef* handle,
                             std::uint8_t* rx,
                             std::uint16_t size,
                             std::uint32_t timeout)
{
    fake::uart_handle = handle;
    fake::uart_timeouts.push_back(timeout);
    if (!timeout || fake::incoming.size() < size)
        return HAL_TIMEOUT;
    for (unsigned i = 0; i < size; ++i) {
        rx[i] = fake::incoming.front();
        fake::incoming.pop_front();
    }
    fake::tick += 2;
    return HAL_OK;
}
#define __HAL_UART_SEND_REQ(handle, request) fake::incoming.clear()
#define __HAL_UART_CLEAR_FLAG(handle, flags) ((handle)->Instance->ISR &= ~(flags))
