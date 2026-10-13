#pragma once
using ULONG = unsigned long;
struct TX_EVENT_FLAGS_GROUP {};
struct TX_THREAD {};
inline constexpr unsigned TX_SUCCESS=0, TX_OR=1, TX_OR_CLEAR=2,
    TX_WAIT_FOREVER=~0u, TX_NO_TIME_SLICE=0, TX_AUTO_START=1;
unsigned tx_event_flags_create(TX_EVENT_FLAGS_GROUP*, char*);
unsigned tx_event_flags_set(TX_EVENT_FLAGS_GROUP*, ULONG, unsigned);
unsigned tx_event_flags_get(TX_EVENT_FLAGS_GROUP*, ULONG, unsigned, ULONG*, ULONG);
unsigned tx_thread_create(TX_THREAD*, char*, void (*)(ULONG), ULONG, void*, ULONG,
                          unsigned, unsigned, ULONG, unsigned);
