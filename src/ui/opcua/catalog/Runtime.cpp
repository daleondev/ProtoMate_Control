#include "Runtime.hpp"
#include "Schema.hpp"
#include "Methods.hpp"
#include <open62541/types_generated.h>
#include <type_traits>
#include <pneumo/logging.hpp>

namespace ui::opcua::catalog
{
    namespace
    {
        UA_String view(std::string_view s) { return { s.size(), reinterpret_cast<UA_Byte*>(const_cast<char*>(s.data())) }; }
        UA_NodeId node(UA_UInt16 ns, std::string_view s)
        { UA_NodeId id{}; id.namespaceIndex = ns; id.identifierType = UA_NODEIDTYPE_STRING; id.identifier.string = view(s); return id; }
        UA_QualifiedName name(UA_UInt16 ns, std::string_view s) { return { ns, view(s) }; }
        UA_LocalizedText label(std::string_view s) { return { UA_STRING(const_cast<char*>("en")), view(s) }; }
        template<class T> const UA_DataType* type()
        {
            if constexpr (std::same_as<T, bool>) return &UA_TYPES[UA_TYPES_BOOLEAN];
            else if constexpr (std::same_as<T, double>) return &UA_TYPES[UA_TYPES_DOUBLE];
            else if constexpr (std::same_as<T, std::uint32_t>) return &UA_TYPES[UA_TYPES_UINT32];
            else if constexpr (std::same_as<T, std::uint64_t>) return &UA_TYPES[UA_TYPES_UINT64];
            else if constexpr (std::same_as<T, std::int32_t>) return &UA_TYPES[UA_TYPES_INT32];
            else if constexpr (std::is_scoped_enum_v<T>) return &UA_TYPES[UA_TYPES_INT32];
            else static_assert(sizeof(T) == 0, "Unsupported exported OPC UA value type");
        }
        template<class T> const UA_DataType* value_type()
        {
            if constexpr (Array<T>::value) {
                if constexpr (std::same_as<typename Array<T>::Element, char>) return &UA_TYPES[UA_TYPES_STRING];
                else return type<typename Array<T>::Element>();
            } else return type<T>();
        }
        template<class T> UA_StatusCode encode(const void* data, UA_Variant* out)
        {
            const auto& value{ *static_cast<const T*>(data) };
            if constexpr (Array<T>::value) {
                if constexpr (std::same_as<typename Array<T>::Element, char>) {
                    const auto end{ std::ranges::find(value, '\0') };
                    auto text{ view({ value.data(), static_cast<std::size_t>(end - value.begin()) }) };
                    return UA_Variant_setScalarCopy(out, &text, &UA_TYPES[UA_TYPES_STRING]);
                } else return UA_Variant_setArrayCopy(out, value.data(), value.size(), value_type<T>());
            } else if constexpr (std::is_scoped_enum_v<T>) {
                const UA_Int32 converted{ static_cast<UA_Int32>(value) };
                return UA_Variant_setScalarCopy(out, &converted, type<T>());
            } else return UA_Variant_setScalarCopy(out, &value, type<T>());
        }
    }
    template<class T>
    void Runtime::variable(T& value, std::string_view id, std::string_view parent, std::string_view title, Variable annotation)
    {
        constexpr bool array{ [] {
            if constexpr (Array<T>::value) return !std::same_as<typename Array<T>::Element, char>;
            else return false;
        }() };
        constexpr std::size_t extent{ [] { if constexpr (Array<T>::value) return Array<T>::extent; else return 0UZ; }() };
        installVariable({ this, &value, encode<T>, annotation.quality }, value_type<T>(), array, extent, id, parent, title, annotation);
        if constexpr (std::is_scoped_enum_v<T>) {
            constexpr auto names{ pnm::meta::enumeration::enumerator_names<T>() };
            constexpr auto values{ pnm::meta::enumeration::underlying_enumerators<T>() };
            std::array<UA_EnumValueType, names.size()> entries{};
            for (unsigned i{}; i < names.size(); ++i) {
                entries[i].value = values[i]; entries[i].displayName = label(names[i]);
            }
            auto property{ UA_VariableAttributes_default };
            property.displayName = label("EnumValues"); property.dataType = UA_TYPES[UA_TYPES_ENUMVALUETYPE].typeId;
            property.accessLevel = UA_ACCESSLEVELMASK_READ; property.valueRank = 1;
            UA_UInt32 dimension{static_cast<UA_UInt32>(entries.size())};
            property.arrayDimensions = &dimension; property.arrayDimensionsSize = 1;
            UA_Variant_setArray(&property.value, entries.data(), entries.size(), &UA_TYPES[UA_TYPES_ENUMVALUETYPE]);
            const auto propertyId{ path(id, "EnumValues") };
            if (!m_error) m_error = UA_Server_addVariableNode(m_server, node(m_namespace, propertyId.data()), node(m_namespace, id),
                UA_NODEID_NUMERIC(0, UA_NS0ID_HASPROPERTY), name(0, "EnumValues"), UA_NODEID_NUMERIC(0, UA_NS0ID_PROPERTYTYPE), property, nullptr, nullptr);
            if (m_error) pnm::log::error("OPC UA property {}: {}", propertyId.data(), UA_StatusCode_name(m_error));
        }
    }
    UA_StatusCode Runtime::install()
    {
        static_assert(valid_schema<Model>());
        // Part 8 is disabled to avoid instantiating unused standard type trees.
        // Keep its standard EUInformation identity and binary encoding for the
        // EngineeringUnits properties. Member layout/definition come from the
        // SDK's generated descriptor, not a project redefinition of the type.
        auto unitType{UA_DataTypeAttributes_default};
        unitType.displayName = label("EUInformation");
        m_error = UA_Server_addDataTypeNode(m_server, UA_TYPES[UA_TYPES_EUINFORMATION].typeId,
            UA_NODEID_NUMERIC(0, UA_NS0ID_STRUCTURE), UA_NODEID_NUMERIC(0, UA_NS0ID_HASSUBTYPE),
            name(0, "EUInformation"), unitType, nullptr, nullptr);
        if (m_error) return m_error;
        auto unitEncoding{UA_ObjectAttributes_default}; unitEncoding.displayName = label("Default Binary");
        m_error = UA_Server_addObjectNode(m_server, UA_TYPES[UA_TYPES_EUINFORMATION].binaryEncodingId,
            UA_NODEID_NULL, UA_NODEID_NULL,
            name(0, "Default Binary"), UA_NODEID_NUMERIC(0, UA_NS0ID_DATATYPEENCODINGTYPE), unitEncoding, nullptr, nullptr);
        if (m_error) return m_error;
        UA_ExpandedNodeId encodingId{}; encodingId.nodeId = UA_TYPES[UA_TYPES_EUINFORMATION].binaryEncodingId;
        m_error = UA_Server_addReference(m_server, UA_TYPES[UA_TYPES_EUINFORMATION].typeId,
            UA_NODEID_NUMERIC(0, UA_NS0ID_HASENCODING), encodingId, true);
        if (m_error) return m_error;
        auto attributes{ UA_ObjectAttributes_default };
        attributes.displayName = label("ProtoMate");
        m_error = UA_Server_addObjectNode(m_server, node(m_namespace, "ProtoMate"), UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
            UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES), name(m_namespace, "ProtoMate"), UA_NODEID_NUMERIC(0, UA_NS0ID_BASEOBJECTTYPE), attributes, nullptr, nullptr);
        if (m_error == UA_STATUSCODE_GOOD) visit(m_model, "ProtoMate", *this);
        if (m_error) pnm::log::error("OPC UA catalog installation failed: {}", UA_StatusCode_name(m_error));
        return m_error;
    }
    void Runtime::object(std::string_view id, std::string_view parent, std::string_view title, Object annotation)
    {
        if (m_error) return;
        auto attributes{ UA_ObjectAttributes_default };
        attributes.displayName = label(title); attributes.description = label(annotation.description);
        m_error = UA_Server_addObjectNode(m_server, node(m_namespace, id), node(m_namespace, parent), UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT),
            name(m_namespace, title), UA_NODEID_NUMERIC(0, UA_NS0ID_BASEOBJECTTYPE), attributes, nullptr, nullptr);
    }
    void Runtime::installVariable(Binding binding, const UA_DataType* datatype, bool array, std::size_t extent,
        std::string_view id, std::string_view parent, std::string_view title, Variable annotation)
    {
        if (m_error) return;
        if (m_count == m_bindings.size()) { m_error = UA_STATUSCODE_BADOUTOFMEMORY; return; }
        auto& stored{ m_bindings[m_count++] }; stored = binding;
        auto attributes{ UA_VariableAttributes_default };
        attributes.displayName = label(title); attributes.description = label(annotation.description);
        attributes.dataType = datatype->typeId;
        attributes.accessLevel = UA_ACCESSLEVELMASK_READ;
        attributes.minimumSamplingInterval = 50;
        attributes.valueRank = array ? 1 : UA_VALUERANK_SCALAR;
        UA_UInt32 dimension{ static_cast<UA_UInt32>(extent) };
        if (array) { attributes.arrayDimensions = &dimension; attributes.arrayDimensionsSize = 1; }
        const UA_DataSource source{ read, nullptr };
        m_error = UA_Server_addDataSourceVariableNode(m_server, node(m_namespace, id), node(m_namespace, parent),
            UA_NODEID_NUMERIC(0, UA_NS0ID_HASCOMPONENT), name(m_namespace, title), UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE),
            attributes, source, &stored, nullptr);
        if (m_error) pnm::log::error("OPC UA node {}: {}", id, UA_StatusCode_name(m_error));
        if (!m_error && annotation.unit != Unit::None) {
            // A project unit namespace avoids pretending custom compound units
            // are official UNECE codes. Clients get standard EUInformation.
            UA_EUInformation eu{};
            eu.namespaceUri = view("urn:protomate:units"); eu.unitId = static_cast<UA_Int32>(annotation.unit);
            eu.displayName = label(unit_name(annotation.unit)); eu.description = eu.displayName;
            auto property{ UA_VariableAttributes_default };
            property.displayName = label("EngineeringUnits"); property.dataType = UA_TYPES[UA_TYPES_EUINFORMATION].typeId;
            property.accessLevel = UA_ACCESSLEVELMASK_READ; property.valueRank = UA_VALUERANK_SCALAR;
            UA_Variant_setScalar(&property.value, &eu, &UA_TYPES[UA_TYPES_EUINFORMATION]);
            const auto propertyId{ path(id, "EngineeringUnits") };
            m_error = UA_Server_addVariableNode(m_server, node(m_namespace, propertyId.data()), node(m_namespace, id),
                UA_NODEID_NUMERIC(0, UA_NS0ID_HASPROPERTY), name(0, "EngineeringUnits"), UA_NODEID_NUMERIC(0, UA_NS0ID_PROPERTYTYPE), property, nullptr, nullptr);
        }
    }
    UA_StatusCode Runtime::read(UA_Server*, const UA_NodeId*, void*, const UA_NodeId*, void* context, UA_Boolean,
        const UA_NumericRange* range, UA_DataValue* out)
    {
        auto& binding{ *static_cast<Binding*>(context) };
        if (range) return UA_STATUSCODE_BADINDEXRANGEINVALID;
        out->status = binding.owner->quality(binding.quality); out->hasStatus = true;
        const auto result{ binding.encode(binding.value, &out->value) };
        out->hasValue = result == UA_STATUSCODE_GOOD;
        // DateTime zero is the OPC UA null timestamp. Mark it supplied so
        // open62541 does not substitute its unsynchronized wall clock when
        // reading a data source. Monotonic age/revision remain explicit nodes.
        out->sourceTimestamp = 0;
        out->hasSourceTimestamp = true;
        return result;
    }
    UA_StatusCode Runtime::quality(Quality policy) const
    {
        if (policy == Quality::Local) return UA_STATUSCODE_GOOD;
        if (!m_sequence) return UA_STATUSCODE_BADWAITINGFORINITIALDATA;
        if (!m_connected) return UA_STATUSCODE_BADNOCOMMUNICATION;
        if (!m_fresh) return UA_STATUSCODE_UNCERTAINLASTUSABLEVALUE;
        if ((policy == Quality::Configured && !m_configured) || (policy == Quality::RobotConfigured && !m_robotConfigured)) return UA_STATUSCODE_BADCONFIGURATIONERROR;
        const bool valid{ policy == Quality::Axis1Feedback ? m_feedback[0] : policy == Quality::Axis2Feedback ? m_feedback[1] :
            policy == Quality::Axis3Feedback ? m_feedback[2] : policy == Quality::RobotFeedback ? m_robotFeedback :
            policy == Quality::Motor1Feedback ? m_motorFeedback[0] : policy == Quality::Motor2Feedback ? m_motorFeedback[1] :
            policy == Quality::Motor3Feedback ? m_motorFeedback[2] : true };
        return valid ? UA_STATUSCODE_GOOD : UA_STATUSCODE_BADNODATA;
    }
    void Runtime::update(const services::Snapshot& state)
    {
        m_model.update(state);
        m_connected = state.connected && !state.failed; m_fresh = state.fresh; m_sequence = state.sequence;
        for (unsigned i{}; i < 3; ++i) m_feedback[i] = state.axes[i].valid && state.configured && state.axes[i].referenced;
        m_robotFeedback = state.feedback_valid;
        m_configured = state.configured; m_robotConfigured = state.robot_configured;
        for (unsigned i{}; i < 3; ++i) m_motorFeedback[i] = state.motor(i).valid;
    }
}
