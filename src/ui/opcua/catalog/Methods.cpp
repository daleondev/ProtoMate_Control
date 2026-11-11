#include "Runtime.hpp"
#include "ui/app/Services.hpp"
#include <pneumo/logging.hpp>

namespace ui::opcua::catalog
{
    void Runtime::configureSessions()
    {
        auto* config{UA_Server_getConfig(m_server)};
        config->context = this;
        m_defaultActivate = config->accessControl.activateSession;
        config->accessControl.activateSession = activate;
        config->accessControl.closeSession = close;
        config->accessControl.getUserExecutable = executable;
    }
    UA_StatusCode Runtime::activate(UA_Server* server, UA_AccessControl* access, const UA_EndpointDescription* endpoint,
        const UA_ByteString* certificate, const UA_NodeId* id, const UA_ExtensionObject* identity, void** context)
    {
        auto& self{*static_cast<Runtime*>(UA_Server_getConfig(server)->context)};
        // Keep the SDK's token-policy checks and anonymous authentication.
        const auto result{self.m_defaultActivate(server, access, endpoint, certificate, id, identity, context)};
        if (result) return result;
        if (*context) {
            auto& session{*static_cast<Session*>(*context)};
            self.m_services.closeRemote(session.source);
            session = {}; session.occupied = true; // reactivation never revives control
            return UA_STATUSCODE_GOOD;
        }
        for (auto& session : self.m_sessions) if (!session.occupied) {
            session = {}; session.occupied = true; *context = &session;
            return UA_STATUSCODE_GOOD;
        }
        return UA_STATUSCODE_BADTOOMANYSESSIONS;
    }
    void Runtime::close(UA_Server* server, UA_AccessControl*, const UA_NodeId*, void* context)
    {
        if (!context) return;
        auto& self{*static_cast<Runtime*>(UA_Server_getConfig(server)->context)};
        auto& session{*static_cast<Session*>(context)};
        self.m_services.closeRemote(session.source);
        session = {};
    }
    UA_Boolean Runtime::executable(UA_Server* server, UA_AccessControl*, const UA_NodeId*, void*, const UA_NodeId*, void* context)
    {
        if (!context) return false;
        auto& self{*static_cast<Runtime*>(UA_Server_getConfig(server)->context)};
        const auto& binding{*static_cast<MethodBinding*>(context)};
        return binding.access != Access::Command || self.m_services.remotePermission();
    }
    void Runtime::installMethod(MethodBinding binding, std::string_view id, std::string_view parent, std::string_view name,
        Method annotation, std::span<const UA_Argument> input, const UA_Argument& output)
    {
        if (m_methodCount == m_methods.size()) {m_error = UA_STATUSCODE_BADOUTOFMEMORY; return;}
        auto& stored{m_methods[m_methodCount++]}; stored = binding;
        auto attributes{UA_MethodAttributes_default};
        attributes.displayName = ua_text(name); attributes.description = ua_text(annotation.description);
        attributes.executable = attributes.userExecutable = true;
        const UA_QualifiedName browseName{m_namespace, ua_string(name)};
        const auto inputId{path(id, "InputArguments")}, outputId{path(id, "OutputArguments")};
        m_error = UA_Server_addMethodNodeEx(m_server, ua_id(m_namespace, id), ua_id(m_namespace, parent),
            UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT), browseName, attributes, dispatch,
            input.size(), input.data(), ua_id(m_namespace, inputId.data()), nullptr,
            1, &output, ua_id(m_namespace, outputId.data()), nullptr, &stored, nullptr);
        if (m_error) pnm::log::error("OPC UA method {}: {}", id, UA_StatusCode_name(m_error));
    }
    UA_StatusCode Runtime::dispatch(UA_Server*, const UA_NodeId*, void* session, const UA_NodeId*, void* method,
        const UA_NodeId*, void*, std::size_t inputs, const UA_Variant* input, std::size_t outputs, UA_Variant* output)
    {
        if (!session || !method) return UA_STATUSCODE_BADSESSIONIDINVALID;
        if (outputs != 1) return UA_STATUSCODE_BADINTERNALERROR;
        auto& binding{*static_cast<MethodBinding*>(method)};
        if (binding.access == Access::Command && !binding.owner->m_services.remotePermission()) return UA_STATUSCODE_BADUSERACCESSDENIED;
        try {return binding.invoke(*binding.owner, *static_cast<Session*>(session), {input, inputs}, output[0]);}
        catch (const StatusError& error) {return error.code;}
        catch (const std::bad_alloc&) {return UA_STATUSCODE_BADOUTOFMEMORY;}
        catch (const std::exception& error) {
            pnm::log::error("OPC UA method failed: {}", error.what());
            return UA_STATUSCODE_BADINTERNALERROR;
        }
    }
}
