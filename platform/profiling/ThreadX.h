#pragma once
#if defined(PROTOMATE_PROFILING)
#define TX_ENABLE_EVENT_TRACE
#if !defined(__ASSEMBLER__)
#include "Trace.h"
#define TX_TRACE_TIME_SOURCE profile_time_us()
#define TX_TRACE_TIME_MASK 0xffffffffUL
#if !defined(PROFILE_NATIVE_INSERT)
#define TX_TRACE_IN_LINE_INSERT(i, a, b, c, d, f)                                                            \
    profile_tx_insert(                                                                                       \
      (uint32_t)(i), (uintptr_t)(a), (uintptr_t)(b), (uintptr_t)(c), (uintptr_t)(d), (uint32_t)(f));
#endif
#endif
#endif
