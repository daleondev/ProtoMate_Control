#include "Server.hpp"
#include "Model.h"
#include "NetX.h"
#include "Wifi.h"
#include <array>
#include <cstddef>

namespace sample {
namespace {
    // CPU-only UA allocations. NetX/SDIO packets use separate AXI buffers.
    alignas(std::max_align_t) std::array<std::byte, 80 * 1024> axiArena;
    alignas(std::max_align_t) __attribute__((section(".dtcm_bss.opcua")))
        std::array<std::byte, 48 * 1024> tcmArena;
    UA_Server* server{};
    SampleModel model{};
}
std::uint32_t startServer() {
    if (server) return UA_STATUSCODE_BADINVALIDSTATE;
    if (!wifi_connected() || !wifi_ip()) return UA_STATUSCODE_BADNOTCONNECTED;
    if (!opcua_memory_install(axiArena.data(), axiArena.size()) ||
        !opcua_memory_add_region(tcmArena.data(), tcmArena.size()))
        return UA_STATUSCODE_BADOUTOFMEMORY;
    UA_ServerConfig config{};
    auto result{ opcua_netx_configure(&config, wifi_ip(), wifi_packet_pool()) };
    if (result == UA_STATUSCODE_GOOD) result = sample_configure(&config, 4840);
    if (result != UA_STATUSCODE_GOOD) { UA_ServerConfig_clear(&config); return result; }
    server = UA_Server_newWithConfig(&config);
    if (!server) { UA_ServerConfig_clear(&config); return UA_STATUSCODE_BADOUTOFMEMORY; }
    result = sample_model_install(server, &model);
    if (result == UA_STATUSCODE_GOOD) result = UA_Server_run_startup(server);
    if (result != UA_STATUSCODE_GOOD) { UA_Server_delete(server); server = nullptr; }
    return result;
}
std::uint32_t stopServer() {
    if (!server) return UA_STATUSCODE_GOOD;
    auto result{ UA_Server_run_shutdown(server) };
    UA_Server_delete(server);
    server = nullptr;
    if (opcua_memory_stats().used && result == UA_STATUSCODE_GOOD)
        result = UA_STATUSCODE_BADINTERNALERROR;
    return result;
}
void pollServer() { if (server) UA_Server_run_iterate(server, true); }
bool serverRunning() { return server != nullptr; }
}
