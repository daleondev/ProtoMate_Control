/* Unit-test seam for the project adapter only. Production compiles against
 * real NetX headers and is tested separately on GIGA. No fake POSIX port. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef unsigned int UINT;
typedef unsigned long ULONG;
typedef struct { ULONG flags; } TX_EVENT_FLAGS_GROUP;
typedef struct { unsigned depth; } TX_MUTEX;
typedef struct { TX_MUTEX nx_ip_protection; } NX_IP;
typedef struct { int unused; } NX_PACKET_POOL;
typedef struct { ULONG nx_packet_length; void *payload; } NX_PACKET;
typedef struct NX_TCP_SOCKET_STRUCT {
    void *nx_tcp_socket_reserved_ptr;
    UINT nx_tcp_socket_state;
    struct NX_TCP_SOCKET_STRUCT *nx_tcp_socket_bound_next;
    struct { unsigned nxd_ip_version; } nx_tcp_socket_connect_ip;
    ULONG nx_tcp_socket_transmit_sent_count, nx_tcp_socket_receive_queue_count;
    void *test;
} NX_TCP_SOCKET;
enum {NX_SUCCESS=0, NX_CONNECTION_PENDING=1, NX_SIZE_ERROR=2, NX_NO_WAIT=0,
      NX_TCP_CLOSED=10, NX_TCP_LISTEN_STATE=11, NX_TCP_SYN_RECEIVED=12,
      NX_TCP_ESTABLISHED=13, NX_TCP_CLOSE_WAIT=14, NX_IP_NORMAL=0,
      NX_FRAGMENT_OKAY=1, NX_IP_TIME_TO_LIVE=64, NX_TCP_PACKET=1,
      TX_SUCCESS=0, TX_OR=1, TX_OR_CLEAR=2, TX_INT_DISABLE=1, TX_WAIT_FOREVER=~0U,
      TX_TIMER_TICKS_PER_SECOND=1000};
UINT tx_event_flags_create(TX_EVENT_FLAGS_GROUP*, char*);
UINT tx_event_flags_delete(TX_EVENT_FLAGS_GROUP*);
UINT tx_event_flags_set(TX_EVENT_FLAGS_GROUP*, ULONG, UINT);
UINT tx_event_flags_get(TX_EVENT_FLAGS_GROUP*, ULONG, UINT, ULONG*, ULONG);
UINT tx_interrupt_control(UINT);
UINT tx_mutex_get(TX_MUTEX*, ULONG);
UINT tx_mutex_put(TX_MUTEX*);
UINT nx_tcp_socket_create(NX_IP*, NX_TCP_SOCKET*, char*, ULONG, ULONG, UINT, ULONG,
                          void(*)(NX_TCP_SOCKET*), void(*)(NX_TCP_SOCKET*));
UINT nx_tcp_socket_delete(NX_TCP_SOCKET*);
UINT nx_tcp_socket_receive_notify(NX_TCP_SOCKET*, void(*)(NX_TCP_SOCKET*));
UINT nx_tcp_socket_establish_notify(NX_TCP_SOCKET*, void(*)(NX_TCP_SOCKET*));
UINT nx_tcp_socket_window_update_notify_set(NX_TCP_SOCKET*, void(*)(NX_TCP_SOCKET*));
UINT nx_tcp_server_socket_listen(NX_IP*, UINT, NX_TCP_SOCKET*, UINT, void(*)(NX_TCP_SOCKET*,UINT));
UINT nx_tcp_server_socket_relisten(NX_IP*, UINT, NX_TCP_SOCKET*);
UINT nx_tcp_server_socket_unlisten(NX_IP*, UINT);
UINT nx_tcp_server_socket_accept(NX_TCP_SOCKET*, ULONG);
UINT nx_tcp_server_socket_unaccept(NX_TCP_SOCKET*);
UINT nx_tcp_socket_disconnect(NX_TCP_SOCKET*, ULONG);
UINT nx_tcp_socket_receive(NX_TCP_SOCKET*, NX_PACKET**, ULONG);
UINT nx_tcp_socket_send(NX_TCP_SOCKET*, NX_PACKET*, ULONG);
UINT nx_packet_allocate(NX_PACKET_POOL*, NX_PACKET**, ULONG, ULONG);
UINT nx_packet_data_append(NX_PACKET*, const void*, ULONG, NX_PACKET_POOL*, ULONG);
UINT nx_packet_data_extract_offset(NX_PACKET*, ULONG, void*, ULONG, ULONG*);
UINT nx_packet_release(NX_PACKET*);
#ifdef __cplusplus
}
#endif
