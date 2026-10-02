#ifndef HAL_PANIC_H
#define HAL_PANIC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#define HAL_PANIC_NOEXCEPT noexcept
#else
#define HAL_PANIC_NOEXCEPT
#endif

typedef struct
{
    const char* message;
    const char* detail;
    const char* file;
    const char* function;
    uint32_t line;
} HalPanicInfo;

/**
 * Fatal-error customization point.
 *
 * The HAL supplies a weak, non-returning default implementation. Applications
 * can provide a strong definition with the same signature to log, persist, or
 * reset instead. It must not return and must be safe in startup and interrupt
 * contexts.
 */
__attribute__((noreturn)) void hal_panic_handler(const HalPanicInfo* info) HAL_PANIC_NOEXCEPT;

/** Project-owned bridge used by generated platform error handlers. */
__attribute__((noreturn)) void hal_error_handler(void) HAL_PANIC_NOEXCEPT;

#ifdef __cplusplus
}
#endif

#undef HAL_PANIC_NOEXCEPT

#endif
