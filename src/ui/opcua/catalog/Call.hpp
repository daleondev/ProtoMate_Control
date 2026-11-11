#pragma once
#include "Records.hpp"
#include <open62541/types.h>
namespace ui { class Services; }
namespace ui::opcua::catalog
{
    struct Session
    {
        struct Previous {std::uint64_t request{}; services::Command command{}; Receipt receipt{};};
        bool occupied{};
        std::uint64_t source{}, lastRequest{};
        unsigned next{};
        std::array<Previous,8> previous;
    };
    class Call
    {
      public:
        Call(Services& services, Session& session) : m_services{services}, m_session{session} {}
        std::uint64_t begin();
        bool heartbeat(std::uint64_t generation, const std::array<std::uint64_t,8>& held);
        Receipt submit(std::uint64_t generation, std::uint64_t request, const services::Command& command);
        Receipt result(std::uint64_t operation);
        JobPage jobs(std::uint32_t offset, bool previews);
        PreviewResult preview(std::uint64_t job);
        DiagnosticPage diagnostics(std::uint32_t offset);
        bool refreshDiagnostics(bool io, bool stacks);
        MessagePage messages(std::uint32_t offset);
        Graphics graphics();
        Configuration configuration();
        MotorDefaults motorDefaults(unsigned motor);
        NetworkDiagnostics network();
        FrontendQueues queues();
      private:
        Services& m_services;
        Session& m_session;
    };
}
