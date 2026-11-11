#include "Echo.h"

/* Temporary lower-layer test. One bounded connection, owned by the sample
 * network thread. NO_WAIT on every network operation. */
static NX_TCP_SOCKET socket;
static NX_PACKET *pending;
static NX_IP *ip;
static int created, accepted;
static ULONG bytes;

UINT sample_echo_start(void) {
    if(created) return NX_ALREADY_ENABLED;
    ip = wifi_ip();
    if(!ip) return NX_NOT_CONNECTED;
    UINT result = nx_tcp_socket_create(ip, &socket, "TCP echo", NX_IP_NORMAL,
        NX_FRAGMENT_OKAY, NX_IP_TIME_TO_LIVE, 4096, NULL, NULL);
    if(result != NX_SUCCESS) return result;
    created = 1;
    result = nx_tcp_server_socket_listen(ip, 5000, &socket, 2, NULL);
    if(result != NX_SUCCESS) { nx_tcp_socket_delete(&socket); created = 0; }
    return result;
}

void sample_echo_poll(void) {
    if(!created) return;
    if(!accepted) {
        if(nx_tcp_server_socket_accept(&socket, NX_NO_WAIT) != NX_SUCCESS) return;
        accepted = 1;
    }
    if(!pending) (void)nx_tcp_socket_receive(&socket, &pending, NX_NO_WAIT);
    if(pending) {
        ULONG length = pending->nx_packet_length;
        if(nx_tcp_socket_send(&socket, pending, NX_NO_WAIT) == NX_SUCCESS) {
            bytes += length;
            pending = NULL;
        }
    }
    if(socket.nx_tcp_socket_state != NX_TCP_ESTABLISHED) {
        if(pending) { nx_packet_release(pending); pending = NULL; }
        nx_tcp_socket_disconnect(&socket, NX_NO_WAIT);
        nx_tcp_server_socket_unaccept(&socket);
        nx_tcp_server_socket_relisten(ip, 5000, &socket);
        accepted = 0;
    }
}

void sample_echo_stop(void) {
    if(!created) return;
    if(pending) { nx_packet_release(pending); pending = NULL; }
    nx_tcp_socket_disconnect(&socket, NX_NO_WAIT);
    nx_tcp_server_socket_unaccept(&socket);
    nx_tcp_socket_delete(&socket);
    nx_tcp_server_socket_unlisten(ip, 5000);
    created = accepted = 0;
}
ULONG sample_echo_bytes(void) { return bytes; }
