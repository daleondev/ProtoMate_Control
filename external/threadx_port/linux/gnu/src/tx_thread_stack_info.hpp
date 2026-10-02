#ifndef TX_THREAD_STACK_INFO_H
#define TX_THREAD_STACK_INFO_H

#include "tx_api.h"

#include <stddef.h>

#ifdef __cplusplus
namespace tx::linux
{
    void refresh_all_threads_stack_info();
}

extern "C" {
#endif

UINT _tx_linux_thread_stack_prepare_host(TX_THREAD* thread_ptr);
VOID* _tx_linux_thread_stack_host_base(TX_THREAD* thread_ptr);
size_t _tx_linux_thread_stack_host_size(TX_THREAD* thread_ptr);
VOID _tx_linux_thread_stack_register(TX_THREAD* thread_ptr);
VOID _tx_linux_thread_stack_unregister(VOID);
VOID _tx_linux_thread_stack_enable_signal_altstack(TX_THREAD* thread_ptr);
VOID _tx_linux_thread_stack_calibrate(TX_THREAD* thread_ptr);
VOID _tx_linux_thread_stack_capture_current(TX_THREAD* thread_ptr);
VOID _tx_linux_thread_stack_capture_signal_context(VOID* context);
VOID _tx_linux_thread_stack_refresh(TX_THREAD* thread_ptr);
VOID _tx_linux_thread_stack_release(TX_THREAD* thread_ptr);
size_t _tx_linux_thread_stack_live_count(VOID);

#ifdef __cplusplus
}
#endif

#endif
