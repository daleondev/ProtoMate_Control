#ifndef HAL_PANIC_H
#define HAL_PANIC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#define HAL_PANIC_NOEXCEPT noexcept
#define HAL_PANIC_NORETURN [[noreturn]]
#else
#define HAL_PANIC_NOEXCEPT
#define HAL_PANIC_NORETURN __attribute__((noreturn))
#endif

typedef struct // NOLINT(modernize-use-using) -- This header is also compiled as C.
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
 * Project panic/error/fault entry points stop STEP generation and disable the
 * motor drivers on STM32 before calling this hook, including strong overrides.
 * Call hal::panic(), hal_error_handler() or hal_fault_handler() to enter that
 * shutdown path; do not call this customization hook directly.
 */
HAL_PANIC_NORETURN void hal_panic_handler(const HalPanicInfo* info) HAL_PANIC_NOEXCEPT;

/** Project-owned bridge used by generated platform error handlers. */
HAL_PANIC_NORETURN void hal_error_handler(void) HAL_PANIC_NOEXCEPT;

/** Project-owned bridge used by Cortex exception handlers. */
HAL_PANIC_NORETURN void hal_fault_handler(const char* fault) HAL_PANIC_NOEXCEPT;

#ifdef __cplusplus
}
#endif

#undef HAL_PANIC_NOEXCEPT
#undef HAL_PANIC_NORETURN

#endif
