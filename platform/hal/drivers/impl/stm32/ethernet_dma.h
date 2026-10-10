#pragma once

#include <stdint.h>

/* CubeMX generates the ETH descriptor tables, but the modern STM32 HAL asks
 * the application (or a network stack) to own and recycle the actual frame
 * buffers through its RX allocation/link callbacks. Apply this attribute to
 * every raw frame buffer that can be handed to the Ethernet DMA.
 *
 * ETH_BUFFER_RAM is non-cacheable, shareable D2 SRAM. The alignment still
 * isolates each buffer on Cortex-M7 cache-line boundaries and keeps the
 * contract safe if cache policy changes later. */
#ifdef __cplusplus
inline constexpr uint32_t ETH_DMA_BUFFER_ALIGNMENT{ 32U };
inline constexpr uint32_t ETH_DMA_FRAME_BUFFER_SIZE{ 1536U };
#else
enum
{
    ETH_DMA_BUFFER_ALIGNMENT = 32U,
    ETH_DMA_FRAME_BUFFER_SIZE = 1536U
};
#endif

#if defined(__GNUC__)
#define ETH_DMA_BUFFER_ATTRIBUTE                                                                             \
    __attribute__((section(".EthBufferSection"), aligned(ETH_DMA_BUFFER_ALIGNMENT)))
#else
#error "Define ETH_DMA_BUFFER_ATTRIBUTE for this toolchain"
#endif

#ifdef __cplusplus
extern "C" {
#endif

extern uint8_t
  __eth_buffer_start__[]; // NOLINT(bugprone-reserved-identifier) -- Symbols provided by the linker script.
extern uint8_t
  __eth_buffer_end__[]; // NOLINT(bugprone-reserved-identifier) -- Symbols provided by the linker script.

#ifdef __cplusplus
}
#endif
