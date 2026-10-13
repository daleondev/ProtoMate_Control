#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void giga_console_connected(int connected);
void giga_console_reset(void);
void giga_console_suspend(void);
void giga_console_resume(void);
void giga_console_deinitialized(void);
void giga_console_receive(const uint8_t *bytes, uint32_t count);
void giga_console_transmitted(void);
#ifdef __cplusplus
}
#endif
