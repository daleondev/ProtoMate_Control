#pragma once

#include "common/diagnostics/Memory.hpp"
#include "common/diagnostics/Profiling.hpp"

#include <pneumo/messaging.hpp>
#include <stop_token>

namespace io::diagnostics
{
    class Server final
    {
      public:
        explicit Server(pnm::msg::Bus& bus);
        // Run on the application's diagnostic worker, outside the ICC context.
        // Stop and join the worker before destroying the server.
        void run(const std::stop_token& stop);

      private:
        pnm::msg::ServiceServer<::diagnostics::ProfileRequest, ::diagnostics::ProfileResponse> m_profile;
        pnm::msg::ServiceServer<::diagnostics::MemoryQuery, ::diagnostics::MemorySnapshot> m_memory;
    };
}
