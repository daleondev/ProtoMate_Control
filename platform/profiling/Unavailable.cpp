// Native simulator coordinator and isolated register-model tests have no
// ThreadX kernel. Recording belongs to the controller/runtime process; these
// hosts deliberately leave the shared code's instrumentation inactive.
#include "profiling/Trace.h"
extern "C" {
uint32_t profile_time_us() noexcept { return 0; }
uint32_t profile_cycles() noexcept { return 0; }
void profile_failure(uint32_t, uint32_t, uint32_t, uintptr_t) noexcept {}
#if defined(PROTOMATE_PROFILING)
void profile_event(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) noexcept {}
uint32_t profile_generation() noexcept { return 0; }
void profile_subscription(uint32_t, uint32_t, uint32_t, uint32_t) noexcept {}
void profile_measure(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) noexcept {}
#endif
}
