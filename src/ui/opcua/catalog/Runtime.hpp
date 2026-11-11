#pragma once
#include "Model.hpp"
#include "Wire.hpp"
#include "Schema.hpp"
#include <open62541/server.h>

namespace ui::opcua::catalog
{
    class Runtime final
    {
      public:
        Runtime(UA_Server* server, UA_UInt16 ns, Services& services)
          : m_server{server}, m_namespace{ns}, m_types{server, ns}, m_services{services} {}
        UA_StatusCode install();
        void update(const services::Snapshot& snapshot);
        void configureSessions();
        unsigned sessions() const { unsigned result{}; for (const auto& session : m_sessions) result += session.occupied; return result; }
        template<std::meta::info Function> void method(std::string_view id, std::string_view parent, std::string_view name, Method annotation);
        Model& model() { return m_model; }
        void object(std::string_view id, std::string_view parent, std::string_view name, Object annotation);
        template<class T> void variable(T& value, std::string_view id, std::string_view parent, std::string_view name, Variable annotation);
      private:
        static constexpr auto CAPACITY{schema_size<Model>()};
        struct Binding
        {
            Runtime* owner{};
            const void* value{};
            UA_StatusCode (*encode)(const void*, UA_Variant*){};
            Quality quality{};
        };
        static UA_StatusCode read(UA_Server*, const UA_NodeId*, void*, const UA_NodeId*, void*, UA_Boolean, const UA_NumericRange*, UA_DataValue*);
        UA_StatusCode quality(Quality policy) const;
        void installVariable(Binding binding, const UA_DataType* type, bool array, std::size_t extent,
            std::string_view id, std::string_view parent, std::string_view name, Variable annotation);
        struct MethodBinding {
            Runtime* owner{};
            Access access{};
            UA_StatusCode (*invoke)(Runtime&, Session&, std::span<const UA_Variant>, UA_Variant&){};
        };
        void installMethod(MethodBinding binding, std::string_view id, std::string_view parent, std::string_view name,
            Method annotation, std::span<const UA_Argument> input, const UA_Argument& output);
        static UA_StatusCode dispatch(UA_Server*, const UA_NodeId*, void*, const UA_NodeId*, void*, const UA_NodeId*, void*, std::size_t, const UA_Variant*, std::size_t, UA_Variant*);
        static UA_StatusCode activate(UA_Server*, UA_AccessControl*, const UA_EndpointDescription*, const UA_ByteString*, const UA_NodeId*, const UA_ExtensionObject*, void**);
        static void close(UA_Server*, UA_AccessControl*, const UA_NodeId*, void*);
        static UA_Boolean executable(UA_Server*, UA_AccessControl*, const UA_NodeId*, void*, const UA_NodeId*, void*);
        UA_Server* m_server;
        UA_UInt16 m_namespace;
        Types m_types;
        Services& m_services;
        decltype(UA_AccessControl::activateSession) m_defaultActivate{};
        std::array<Session,2> m_sessions;
        std::array<MethodBinding,CAPACITY.methods> m_methods;
        unsigned m_methodCount{};
        Model m_model{};
        std::array<Binding, CAPACITY.variables> m_bindings;
        unsigned m_count{};
        UA_StatusCode m_error{};
        bool m_connected{}, m_fresh{};
        std::uint64_t m_sequence{};
        std::array<bool, 3> m_feedback{};
        bool m_robotFeedback{}, m_configured{}, m_robotConfigured{};
        std::array<bool,3> m_motorFeedback{};
    };
}
