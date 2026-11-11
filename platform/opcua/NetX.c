#include "NetX.h"
#include "timer.h"
#include <open62541/plugin/log_stdout.h>
#include <string.h>

#define CONNECTIONS 3
#define BUFFER_SIZE 8192
#define SEND_QUEUE 2
#define WAKE 1UL

typedef struct Loop Loop;
typedef struct Manager Manager;
typedef struct {
    NX_TCP_SOCKET socket;
    Manager *owner;
    uintptr_t id;
    UA_Boolean created, accepted, closing;
    UA_DateTime closeDeadline;
    void *context;
    UA_ByteString outgoing[SEND_QUEUE];
    size_t sent;
    unsigned first, count;
} Connection;

struct Loop {
    UA_EventLoop api;
    UA_Timer timer;
    TX_EVENT_FLAGS_GROUP notifications;
    UA_DelayedCallback *head, *tail, *executing;
};

struct Manager {
    UA_ConnectionManager api;
    NX_IP *ip;
    NX_PACKET_POOL *pool;
    Connection connections[CONNECTIONS];
    Connection *listening;
    uintptr_t nextId;
    UA_Boolean listenOpen;
    UA_UInt16 port;
    void *application, *listenContext;
    UA_ConnectionManager_connectionCallback callback;
    UA_Byte receiveBuffer[BUFFER_SIZE];
};

static Loop *loopOf(Manager *m) { return (Loop *)m->api.eventSource.eventLoop; }
static void wake(Loop *loop) { tx_event_flags_set(&loop->notifications, WAKE, TX_OR); }
static void notify(NX_TCP_SOCKET *socket) {
    Connection *c = socket->nx_tcp_socket_reserved_ptr;
    if(c) wake(loopOf(c->owner));
}
static void listenNotify(NX_TCP_SOCKET *socket, UINT port) { (void)port; notify(socket); }
static void setState(Loop *loop, UA_EventLoopState state) {
    *(volatile UA_EventLoopState *)(uintptr_t)&loop->api.state = state;
}
static Connection *lookup(Manager *m, uintptr_t id) {
    for(unsigned i = 0; i < CONNECTIONS; ++i)
        if(m->connections[i].created && m->connections[i].accepted && m->connections[i].id == id)
            return &m->connections[i];
    return NULL;
}

static UA_StatusCode listenNext(Manager *m, UA_Boolean first) {
    if(!m->listenOpen || m->listening) return UA_STATUSCODE_GOOD;
    for(unsigned i = 0; i < CONNECTIONS; ++i) {
        Connection *c = &m->connections[i];
        if(c->created) continue;
        memset(c, 0, sizeof(*c));
        c->owner = m;
        UINT result = nx_tcp_socket_create(m->ip, &c->socket, "OPC UA", NX_IP_NORMAL,
            NX_FRAGMENT_OKAY, NX_IP_TIME_TO_LIVE, BUFFER_SIZE, NULL, notify);
        if(result != NX_SUCCESS) {
            UA_LOG_ERROR(loopOf(m)->api.logger, UA_LOGCATEGORY_NETWORK,
                         "NetX socket creation failed: 0x%x", result);
            return UA_STATUSCODE_BADRESOURCEUNAVAILABLE;
        }
        c->created = true;
        c->socket.nx_tcp_socket_reserved_ptr = c;
        nx_tcp_socket_receive_notify(&c->socket, notify);
        nx_tcp_socket_establish_notify(&c->socket, notify);
        nx_tcp_socket_window_update_notify_set(&c->socket, notify);
        result = first ? nx_tcp_server_socket_listen(m->ip, m->port, &c->socket, 2, listenNotify)
                       : nx_tcp_server_socket_relisten(m->ip, m->port, &c->socket);
        if(result != NX_SUCCESS && result != NX_CONNECTION_PENDING) {
            UA_LOG_ERROR(loopOf(m)->api.logger, UA_LOGCATEGORY_NETWORK,
                         "NetX %s port %u failed: 0x%x", first ? "listen" : "relisten", m->port, result);
            nx_tcp_socket_delete(&c->socket);
            c->created = false;
            return UA_STATUSCODE_BADCOMMUNICATIONERROR;
        }
        m->listening = c;
        return UA_STATUSCODE_GOOD;
    }
    return UA_STATUSCODE_GOOD; // All accepted slots occupied; TCP backlog is bounded.
}

static void report(Manager *m, Connection *c, UA_ConnectionState state, UA_ByteString data) {
    m->callback(&m->api, c->id, m->application, &c->context, state, &UA_KEYVALUEMAP_NULL, data);
}

static void destroyConnection(Manager *m, Connection *c) {
    nx_tcp_socket_disconnect(&c->socket, NX_NO_WAIT);
    nx_tcp_server_socket_unaccept(&c->socket);
    nx_tcp_socket_delete(&c->socket);
    for(unsigned i = 0; i < SEND_QUEUE; ++i) UA_ByteString_clear(&c->outgoing[i]);
    if(m->listening == c) m->listening = NULL;
    const UA_Boolean accepted = c->accepted;
    c->created = false;
    if(accepted) report(m, c, UA_CONNECTIONSTATE_CLOSING, UA_BYTESTRING_NULL);
    c->accepted = false;
}

static UA_StatusCode openConnection(UA_ConnectionManager *api, const UA_KeyValueMap *params,
    void *application, void *context, UA_ConnectionManager_connectionCallback callback) {
    Manager *m = (Manager *)api;
    const UA_Boolean *listen = UA_KeyValueMap_getScalar(params, UA_QUALIFIEDNAME(0,"listen"), &UA_TYPES[UA_TYPES_BOOLEAN]);
    const UA_UInt16 *port = UA_KeyValueMap_getScalar(params, UA_QUALIFIEDNAME(0,"port"), &UA_TYPES[UA_TYPES_UINT16]);
    const UA_Boolean *validate = UA_KeyValueMap_getScalar(params, UA_QUALIFIEDNAME(0,"validate"), &UA_TYPES[UA_TYPES_BOOLEAN]);
    if(!listen || !*listen || !port || !*port) return UA_STATUSCODE_BADNOTSUPPORTED;
    if(validate && *validate) return UA_STATUSCODE_GOOD;
    if(m->listenOpen) return UA_STATUSCODE_BADRESOURCEUNAVAILABLE;
    m->port = *port;
    m->listenOpen = true;
    m->callback = callback;
    m->application = application;
    m->listenContext = context;
    UA_StatusCode result = listenNext(m, true);
    if(result != UA_STATUSCODE_GOOD) { m->listenOpen = false; return result; }
    UA_KeyValueMap properties = UA_KEYVALUEMAP_NULL;
    UA_Boolean yes = true;
    result = UA_KeyValueMap_setScalar(&properties, UA_QUALIFIEDNAME(0,"listen"), &yes, &UA_TYPES[UA_TYPES_BOOLEAN]);
    if(result == UA_STATUSCODE_GOOD)
        result = UA_KeyValueMap_setScalar(&properties, UA_QUALIFIEDNAME(0,"listen-port"), port, &UA_TYPES[UA_TYPES_UINT16]);
    if(result == UA_STATUSCODE_GOOD)
        callback(api, 1, application, &m->listenContext, UA_CONNECTIONSTATE_ESTABLISHED, &properties, UA_BYTESTRING_NULL);
    UA_KeyValueMap_clear(&properties);
    if(result != UA_STATUSCODE_GOOD) {
        m->listenOpen = false;
        destroyConnection(m, m->listening);
        nx_tcp_server_socket_unlisten(m->ip, m->port);
    }
    return result;
}

static UA_StatusCode closeConnection(UA_ConnectionManager *api, uintptr_t id) {
    Manager *m = (Manager *)api;
    if(id == 1 && m->listenOpen) {
        m->listenOpen = false;
        /* NO_WAIT accept puts even an idle socket into SYN_RECEIVED. NetX
         * unlisten rejects that state; disconnect/unaccept must run first. */
        if(m->listening) destroyConnection(m, m->listening);
        UINT status = nx_tcp_server_socket_unlisten(m->ip, m->port);
        if(status != NX_SUCCESS)
            UA_LOG_ERROR(loopOf(m)->api.logger, UA_LOGCATEGORY_NETWORK,
                         "NetX unlisten port %u failed: 0x%x", m->port, status);
        m->callback(api, 1, m->application, &m->listenContext, UA_CONNECTIONSTATE_CLOSING,
            &UA_KEYVALUEMAP_NULL, UA_BYTESTRING_NULL);
        return UA_STATUSCODE_GOOD;
    }
    Connection *c = lookup(m, id);
    if(!c) return UA_STATUSCODE_BADNOTFOUND;
    if(!c->closing) c->closeDeadline = UA_DateTime_nowMonotonic() + UA_DATETIME_SEC;
    c->closing = true;
    wake(loopOf(m));
    return UA_STATUSCODE_GOOD;
}

static UA_StatusCode allocateBuffer(UA_ConnectionManager *api, uintptr_t id, UA_ByteString *buffer, size_t size) {
    if(!lookup((Manager *)api, id) || size > BUFFER_SIZE) return UA_STATUSCODE_BADCOMMUNICATIONERROR;
    return UA_ByteString_allocBuffer(buffer, size);
}
static void freeBuffer(UA_ConnectionManager *api, uintptr_t id, UA_ByteString *buffer) {
    (void)api; (void)id; UA_ByteString_clear(buffer);
}
static UA_StatusCode sendBuffer(UA_ConnectionManager *api, uintptr_t id, const UA_KeyValueMap *params, UA_ByteString *buffer) {
    (void)params;
    Manager *m = (Manager *)api;
    Connection *c = lookup(m, id);
    if(!c || c->closing || c->count == SEND_QUEUE || buffer->length > BUFFER_SIZE) {
        UA_ByteString_clear(buffer);
        /* The engine marks its SecureChannel CLOSING on send failure and
         * expects the transport to deliver the eventual closing callback.
         * Merely returning an error strands that channel, including shutdown. */
        if(c) closeConnection(api, id);
        return UA_STATUSCODE_BADRESOURCEUNAVAILABLE;
    }
    c->outgoing[(c->first + c->count) % SEND_QUEUE] = *buffer;
    *buffer = UA_BYTESTRING_NULL;
    ++c->count;
    wake(loopOf(m));
    return UA_STATUSCODE_GOOD;
}

static void service(Manager *m) {
    Connection *pending = m->listening;
    if(pending && nx_tcp_server_socket_accept(&pending->socket, NX_NO_WAIT) == NX_SUCCESS) {
        m->listening = NULL;
        pending->accepted = true;
        pending->id = ++m->nextId;
        pending->context = m->listenContext;
        report(m, pending, UA_CONNECTIONSTATE_ESTABLISHED, UA_BYTESTRING_NULL);
    } else if(pending && (pending->socket.nx_tcp_socket_state == NX_TCP_CLOSED ||
                         pending->socket.nx_tcp_socket_state == NX_TCP_CLOSE_WAIT)) {
        /* A peer can finish its handshake and disconnect before our next turn.
         * accept(NO_WAIT) then reports NOT_LISTEN_STATE, not success. Reclaim
         * this unpublished socket or it permanently consumes the listener. */
        destroyConnection(m, pending);
    }
    for(unsigned i = 0; i < CONNECTIONS; ++i) {
        Connection *c = &m->connections[i];
        if(!c->created || !c->accepted) continue;
        if(c->socket.nx_tcp_socket_state != NX_TCP_ESTABLISHED &&
           c->socket.nx_tcp_socket_state != NX_TCP_CLOSE_WAIT) {
            destroyConnection(m, c);
            continue;
        }
        /* A bounded number of packets per event-loop turn keeps timers live. */
        for(unsigned packets = 0; packets < 8 && !c->closing; ++packets) {
            NX_PACKET *packet = NULL;
            if(nx_tcp_socket_receive(&c->socket, &packet, NX_NO_WAIT) != NX_SUCCESS) break;
            ULONG length = packet->nx_packet_length;
            ULONG copied = 0;
            UINT result = length <= BUFFER_SIZE
                ? nx_packet_data_extract_offset(packet, 0, m->receiveBuffer, length, &copied) : NX_SIZE_ERROR;
            nx_packet_release(packet);
            if(result != NX_SUCCESS || copied != length) { closeConnection(&m->api, c->id); break; }
            report(m, c, UA_CONNECTIONSTATE_ESTABLISHED, (UA_ByteString){length, m->receiveBuffer});
        }
        if(c->socket.nx_tcp_socket_state == NX_TCP_CLOSE_WAIT &&
           !c->socket.nx_tcp_socket_receive_queue_count) closeConnection(&m->api, c->id);
        for(unsigned packets = 0; packets < 8 && c->count; ++packets) {
            UA_ByteString *buffer = &c->outgoing[c->first];
            ULONG length = (ULONG)(buffer->length - c->sent);
            if(length > 1024) length = 1024;
            NX_PACKET *packet = NULL;
            if(nx_packet_allocate(m->pool, &packet, NX_TCP_PACKET, NX_NO_WAIT) != NX_SUCCESS) break;
            UINT result = nx_packet_data_append(packet, buffer->data + c->sent, length, m->pool, NX_NO_WAIT);
            if(result == NX_SUCCESS) result = nx_tcp_socket_send(&c->socket, packet, NX_NO_WAIT);
            if(result != NX_SUCCESS) { nx_packet_release(packet); break; }
            c->sent += length;
            if(c->sent == buffer->length) {
                UA_ByteString_clear(buffer);
                c->first = (c->first + 1) % SEND_QUEUE;
                --c->count;
                c->sent = 0;
            }
        }
        if(c->closing && ((!c->count && !c->socket.nx_tcp_socket_transmit_sent_count) ||
                         UA_DateTime_nowMonotonic() >= c->closeDeadline)) destroyConnection(m, c);
    }
    if(m->listenOpen) (void)listenNext(m, false);
    if(m->api.eventSource.state == UA_EVENTSOURCESTATE_STOPPING) {
        UA_Boolean active = false;
        for(unsigned i = 0; i < CONNECTIONS; ++i) active |= m->connections[i].created;
        if(!active) m->api.eventSource.state = UA_EVENTSOURCESTATE_STOPPED;
    }
}

static UA_StatusCode sourceStart(UA_EventSource *source) { source->state = UA_EVENTSOURCESTATE_STARTED; return UA_STATUSCODE_GOOD; }
static void sourceStop(UA_EventSource *source) {
    Manager *m = (Manager *)source;
    source->state = UA_EVENTSOURCESTATE_STOPPING;
    if(m->listenOpen) closeConnection(&m->api, 1);
    for(unsigned i = 0; i < CONNECTIONS; ++i)
        if(m->connections[i].accepted) closeConnection(&m->api, m->connections[i].id);
    service(m);
}
static UA_StatusCode sourceFree(UA_EventSource *source) {
    if(source->state != UA_EVENTSOURCESTATE_STOPPED && source->state != UA_EVENTSOURCESTATE_FRESH)
        return UA_STATUSCODE_BADINVALIDSTATE;
    UA_String_clear(&source->name);
    UA_KeyValueMap_clear(&source->params);
    UA_String_clear(&((Manager *)source)->api.protocol);
    UA_free(source);
    return UA_STATUSCODE_GOOD;
}
static UA_StatusCode registerSource(UA_EventLoop *el, UA_EventSource *source) {
    if(el->eventSources || source->eventSourceType != UA_EVENTSOURCETYPE_CONNECTIONMANAGER)
        return UA_STATUSCODE_BADNOTSUPPORTED;
    el->eventSources = source;
    source->eventLoop = el;
    return el->state == UA_EVENTLOOPSTATE_STARTED ? source->start(source) : UA_STATUSCODE_GOOD;
}
static UA_StatusCode deregisterSource(UA_EventLoop *el, UA_EventSource *source) {
    if(el->eventSources != source || source->state != UA_EVENTSOURCESTATE_STOPPED)
        return UA_STATUSCODE_BADINVALIDSTATE;
    el->eventSources = NULL;
    source->eventLoop = NULL;
    return UA_STATUSCODE_GOOD;
}
static UA_StatusCode start(UA_EventLoop *el) {
    if(el->eventSources) {
        UA_StatusCode result = el->eventSources->start(el->eventSources);
        if(result != UA_STATUSCODE_GOOD) return result;
    }
    setState((Loop *)el, UA_EVENTLOOPSTATE_STARTED);
    return UA_STATUSCODE_GOOD;
}
static void stop(UA_EventLoop *el) {
    setState((Loop *)el, UA_EVENTLOOPSTATE_STOPPING);
    if(el->eventSources) el->eventSources->stop(el->eventSources);
    wake((Loop *)el);
}
static void cancel(UA_EventLoop *el) { wake((Loop *)el); }
static UA_DateTime now(UA_EventLoop *el) { (void)el; return UA_DateTime_now(); }
static UA_DateTime monotonic(UA_EventLoop *el) { (void)el; return UA_DateTime_nowMonotonic(); }
static UA_Int64 offset(UA_EventLoop *el) { (void)el; return 0; }
static void lock(UA_EventLoop *el) { (void)el; } // Single-owner UA_MULTITHREADING=0.
static UA_DateTime nextTimer(UA_EventLoop *el) {
    Loop *loop = (Loop *)el;
    return loop->head ? UA_DateTime_nowMonotonic() : UA_Timer_next(&loop->timer);
}
static UA_StatusCode addTimer(UA_EventLoop *el, UA_Callback cb, void *app, void *data,
    UA_Double interval, UA_DateTime *base, UA_TimerPolicy policy, UA_UInt64 *id) {
    return UA_Timer_add(&((Loop *)el)->timer, cb, app, data, interval, UA_DateTime_nowMonotonic(), base, policy, id);
}
static UA_StatusCode modifyTimer(UA_EventLoop *el, UA_UInt64 id, UA_Double interval, UA_DateTime *base, UA_TimerPolicy policy) {
    return UA_Timer_modify(&((Loop *)el)->timer, id, interval, UA_DateTime_nowMonotonic(), base, policy);
}
static void removeTimer(UA_EventLoop *el, UA_UInt64 id) { UA_Timer_remove(&((Loop *)el)->timer, id); }
static void addDelayed(UA_EventLoop *el, UA_DelayedCallback *callback) {
    Loop *loop = (Loop *)el;
    UINT previous = tx_interrupt_control(TX_INT_DISABLE);
    callback->next = NULL;
    if(loop->tail) loop->tail->next = callback; else loop->head = callback;
    loop->tail = callback;
    tx_interrupt_control(previous);
    wake(loop);
}
static void removeDelayed(UA_EventLoop *el, UA_DelayedCallback *callback) {
    Loop *loop = (Loop *)el;
    UINT previous = tx_interrupt_control(TX_INT_DISABLE);
    UA_DelayedCallback **p = &loop->head, *tail = NULL;
    while(*p) {
        if(*p == callback) *p = callback->next;
        else { tail = *p; p = &(*p)->next; }
    }
    loop->tail = tail;
    p = &loop->executing;
    while(*p) {
        if(*p == callback) { *p = callback->next; break; }
        p = &(*p)->next;
    }
    tx_interrupt_control(previous);
}
static UA_StatusCode run(UA_EventLoop *el, UA_UInt32 timeout) {
    Loop *loop = (Loop *)el;
    UA_Timer_process(&loop->timer, UA_DateTime_nowMonotonic());
    UINT previous = tx_interrupt_control(TX_INT_DISABLE);
    loop->executing = loop->head;
    loop->head = loop->tail = NULL;
    tx_interrupt_control(previous);
    while(loop->executing) {
        UA_DelayedCallback *callback = loop->executing;
        loop->executing = callback->next;
        callback->callback(callback->application, callback->context);
    }
    if(el->eventSources) service((Manager *)el->eventSources);
    if(el->state == UA_EVENTLOOPSTATE_STOPPING && !loop->head &&
       (!el->eventSources || el->eventSources->state == UA_EVENTSOURCESTATE_STOPPED)) {
        setState(loop, UA_EVENTLOOPSTATE_STOPPED);
        return UA_STATUSCODE_GOOD;
    }
    UA_DateTime remaining = nextTimer(el) - UA_DateTime_nowMonotonic();
    if(remaining <= 0) timeout = 0;
    else if(remaining / UA_DATETIME_MSEC < timeout) timeout = (UA_UInt32)(remaining / UA_DATETIME_MSEC);
    if(timeout > 20) timeout = 20; // Bounded fallback for dropped/coalesced network notifications.
    if(timeout && !loop->head) {
        ULONG flags;
        tx_event_flags_get(&loop->notifications, WAKE, TX_OR_CLEAR, &flags,
            (timeout * TX_TIMER_TICKS_PER_SECOND + 999) / 1000);
    }
    return UA_STATUSCODE_GOOD;
}
static UA_StatusCode freeLoop(UA_EventLoop *el) {
    if(el->state != UA_EVENTLOOPSTATE_STOPPED && el->state != UA_EVENTLOOPSTATE_FRESH)
        return UA_STATUSCODE_BADINVALIDSTATE;
    Loop *loop = (Loop *)el;
    if(el->eventSources) {
        UA_StatusCode status = el->eventSources->free(el->eventSources);
        if(status != UA_STATUSCODE_GOOD) return status;
    }
    UA_Timer_clear(&loop->timer);
    /* Server deletion can enqueue final frees after the loop has stopped. */
    while(loop->head) {
        UA_DelayedCallback *callback = loop->head;
        loop->head = callback->next;
        if(!loop->head) loop->tail = NULL;
        callback->callback(callback->application, callback->context);
    }
    UA_KeyValueMap_clear(&el->params);
    tx_event_flags_delete(&loop->notifications);
    UA_free(loop);
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode opcua_netx_configure(UA_ServerConfig *config, NX_IP *ip, NX_PACKET_POOL *pool) {
    if(!config || !ip || !pool || config->eventLoop) return UA_STATUSCODE_BADINVALIDARGUMENT;
    Loop *loop = UA_calloc(1, sizeof(*loop));
    Manager *m = UA_calloc(1, sizeof(*m));
    if(!loop || !m) { UA_free(loop); UA_free(m); return UA_STATUSCODE_BADOUTOFMEMORY; }
    if(tx_event_flags_create(&loop->notifications, "OPC UA events") != TX_SUCCESS) {
        UA_free(loop); UA_free(m); return UA_STATUSCODE_BADRESOURCEUNAVAILABLE;
    }
    UA_Timer_init(&loop->timer);
    UA_EventLoop api = {.logger = UA_Log_Stdout, .start = start, .stop = stop, .free = freeLoop,
        .run = run, .cancel = cancel, .dateTime_now = now, .dateTime_nowMonotonic = monotonic,
        .dateTime_localTimeUtcOffset = offset, .nextTimer = nextTimer, .addTimer = addTimer,
        .modifyTimer = modifyTimer, .removeTimer = removeTimer, .addDelayedCallback = addDelayed,
        .removeDelayedCallback = removeDelayed, .registerEventSource = registerSource,
        .deregisterEventSource = deregisterSource, .lock = lock, .unlock = lock};
    memcpy(&loop->api, &api, sizeof(api));
    m->ip = ip; m->pool = pool; m->nextId = 1;
    m->api.eventSource.eventSourceType = UA_EVENTSOURCETYPE_CONNECTIONMANAGER;
    m->api.eventSource.name = UA_STRING_ALLOC("NetX TCP");
    m->api.eventSource.start = sourceStart;
    m->api.eventSource.stop = sourceStop;
    m->api.eventSource.free = sourceFree;
    m->api.protocol = UA_STRING_ALLOC("tcp");
    m->api.openConnection = openConnection;
    m->api.closeConnection = closeConnection;
    m->api.sendWithConnection = sendBuffer;
    m->api.allocNetworkBuffer = allocateBuffer;
    m->api.freeNetworkBuffer = freeBuffer;
    UA_UInt32 size = BUFFER_SIZE;
    UA_StatusCode result = UA_KeyValueMap_setScalar(&m->api.eventSource.params,
        UA_QUALIFIEDNAME(0,"recv-bufsize"), &size, &UA_TYPES[UA_TYPES_UINT32]);
    if(result == UA_STATUSCODE_GOOD)
        result = UA_KeyValueMap_setScalar(&m->api.eventSource.params,
            UA_QUALIFIEDNAME(0,"send-bufsize"), &size, &UA_TYPES[UA_TYPES_UINT32]);
    if(!m->api.eventSource.name.data || !m->api.protocol.data) result = UA_STATUSCODE_BADOUTOFMEMORY;
    if(result != UA_STATUSCODE_GOOD) { sourceFree(&m->api.eventSource); freeLoop(&loop->api); return result; }
    registerSource(&loop->api, &m->api.eventSource);
    config->eventLoop = &loop->api;
    return UA_STATUSCODE_GOOD;
}
