#include "Model.h"
#include "Memory.h"
#include <open62541/plugin/log_stdout.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

static volatile sig_atomic_t running = 1;
static _Alignas(max_align_t) unsigned char arena[128 * 1024];
UA_StatusCode opcua_posix_configure(UA_ServerConfig *config);
static void stop(int signal) { (void)signal; running = 0; }

int main(int argc, char **argv) {
    unsigned long port = 4840;
    if(argc == 2) {
        char *end;
        errno = 0;
        port = strtoul(argv[1], &end, 10);
        if(errno || *end || port < 1 || port > 65535) return EXIT_FAILURE;
    } else if(argc != 1) return EXIT_FAILURE;
    if(!opcua_memory_install(arena, sizeof(arena))) return EXIT_FAILURE;
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    UA_ServerConfig config = {0};
    UA_StatusCode status = opcua_posix_configure(&config);
    if(status == UA_STATUSCODE_GOOD) status = sample_configure(&config, (UA_UInt16)port);
    if(status != UA_STATUSCODE_GOOD) {
        fprintf(stderr, "Configuration failed: %s\n", UA_StatusCode_name(status));
        OpcUaMemoryStats memory = opcua_memory_stats();
        fprintf(stderr, "used=%zu peak=%zu failures=%zu\n", memory.used, memory.peak, memory.failures);
        UA_ServerConfig_clear(&config);
        return EXIT_FAILURE;
    }
    UA_Server *server = UA_Server_newWithConfig(&config);
    if(!server) { UA_ServerConfig_clear(&config); return EXIT_FAILURE; }
    SampleModel model;
    status = sample_model_install(server, &model);
    if(status == UA_STATUSCODE_GOOD) status = UA_Server_run_startup(server);
    if(status == UA_STATUSCODE_GOOD) {
        printf("READY opc.tcp://localhost:%lu namespace=%u (dummy data, anonymous, no encryption)\n",
               port, model.namespaceIndex);
        fflush(stdout);
        while(running) UA_Server_run_iterate(server, true);
        status = UA_Server_run_shutdown(server);
    }
    UA_Server_delete(server);
    OpcUaMemoryStats memory = opcua_memory_stats();
    printf("MEMORY capacity=%zu used=%zu peak=%zu failures=%zu allocations=%zu\n",
        memory.capacity, memory.used, memory.peak, memory.failures, memory.allocations);
    if(status != UA_STATUSCODE_GOOD) fprintf(stderr, "Server failed: %s\n", UA_StatusCode_name(status));
    return status == UA_STATUSCODE_GOOD && memory.used == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
