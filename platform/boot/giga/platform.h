#pragma once
#include "format.h"
#include "stm32h7xx_hal.h"
#ifdef __cplusplus
extern "C" {
#endif
void giga_safe_outputs(void);
void giga_configure_mpu(uint32_t ram_debug);
void giga_release_m4(void);
void giga_memories_initialize(void);
int giga_sdram_test(void);
void giga_fail(uint32_t reason) __attribute__((noreturn));
void giga_jump(uint32_t entry, uint32_t stack) __attribute__((noreturn));
void Error_Handler(void);
extern volatile uint32_t giga_boot_status;
#ifdef __cplusplus
}
#endif
