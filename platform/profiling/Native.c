/* This translation unit alone sees the original ThreadX insertion macro.
 * All other sources share this out-of-line copy rather than inflating M4 RAM. */
#define PROFILE_NATIVE_INSERT
#include "Trace.h"
#include <tx_api.h>
#include <tx_trace.h>

void profile_tx_insert(uint32_t event, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uint32_t filter)
{
    TX_TRACE_IN_LINE_INSERT(event, a, b, c, d, filter)
    if (_tx_trace_buffer_current_ptr != TX_NULL && (_tx_trace_event_enable_bits & filter))
        ++protomate_profile.status.events;
}
