#pragma once
#include "Wifi.h"
#ifdef __cplusplus
extern "C" {
#endif
UINT sample_echo_start(void);
void sample_echo_poll(void);
void sample_echo_stop(void);
ULONG sample_echo_bytes(void);
#ifdef __cplusplus
}
#endif
