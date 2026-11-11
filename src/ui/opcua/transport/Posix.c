#include <open62541/server.h>
#include <open62541/plugin/log_stdout.h>

/* Only TCP is needed. Default POSIX configuration also installs UDP and
 * allocates much larger buffers before application limits take effect. */
UA_StatusCode opcua_posix_configure(UA_ServerConfig *config) {
    config->eventLoop = UA_EventLoop_new_POSIX(UA_Log_Stdout);
    if(!config->eventLoop) return UA_STATUSCODE_BADOUTOFMEMORY;
    UA_ConnectionManager *tcp = UA_ConnectionManager_new_POSIX_TCP(UA_STRING("sample tcp"));
    if(!tcp) return UA_STATUSCODE_BADOUTOFMEMORY;
    const UA_UInt32 size = 8192;
    UA_StatusCode result = UA_KeyValueMap_setScalar(&tcp->eventSource.params,
        UA_QUALIFIEDNAME(0, "recv-bufsize"), &size, &UA_TYPES[UA_TYPES_UINT32]);
    if(result == UA_STATUSCODE_GOOD)
        result = UA_KeyValueMap_setScalar(&tcp->eventSource.params,
            UA_QUALIFIEDNAME(0, "send-bufsize"), &size, &UA_TYPES[UA_TYPES_UINT32]);
    if(result == UA_STATUSCODE_GOOD)
        result = config->eventLoop->registerEventSource(config->eventLoop, &tcp->eventSource);
    if(result != UA_STATUSCODE_GOOD) tcp->eventSource.free(&tcp->eventSource);
    return result;
}
