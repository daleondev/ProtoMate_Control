#pragma once
#include "ui/opcua/util/Memory.h"
#include <cstdint>
namespace sample {
    // Called only by the network/server worker, never USB or network callbacks.
    std::uint32_t startServer();
    std::uint32_t stopServer();
    void pollServer();
    bool serverRunning();
}
