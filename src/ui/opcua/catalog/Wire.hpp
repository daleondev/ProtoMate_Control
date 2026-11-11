#pragma once
#include "Annotations.hpp"
#include "Schema.hpp"
#include <open62541/server.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace ui::opcua::catalog
{
    template<class T> struct Array : std::false_type {};
    template<class T, std::size_t N> struct Array<std::array<T, N>> : std::true_type
    { using Element = T; static constexpr std::size_t extent{ N }; };

    template<class T> struct Wire;
    template<class T> struct Sequence { std::size_t size{}; T* data{}; };
    template<class T> requires (std::is_arithmetic_v<T> || std::is_enum_v<T>)
    struct Wire<T> { using Type = std::conditional_t<std::is_enum_v<T>, UA_Int32, T>; };
    template<std::size_t N> struct Wire<std::array<char, N>> { using Type = UA_String; };
    template<class T, std::size_t N> requires (!std::same_as<T, char>)
    struct Wire<std::array<T, N>> { using Type = Sequence<typename Wire<T>::Type>; };
    template<class T> requires (annotation<Record>(^^T).has_value())
    struct Wire<T>
    {
        struct Type;
        consteval {
            static constexpr auto source{ fields<T>() };
            std::vector<std::meta::info> members;
            template for (constexpr auto field : source) {
                static_assert(annotation<Field>(field).has_value(), "Every wire record field must be explicitly exported");
                using Value = typename[:std::meta::type_of(field):];
                constexpr bool numeric{[] {
                    if constexpr (Array<Value>::value) return std::is_floating_point_v<typename Array<Value>::Element>;
                    else return std::is_floating_point_v<Value>;
                }()};
                static_assert(!annotation<Record>(^^T)->input || !numeric || annotation<Bounds>(field).has_value(), "Input numeric fields require explicit bounds");
                members.push_back(std::meta::data_member_spec(^^typename Wire<Value>::Type,
                    {.name = std::meta::identifier_of(field)}));
            }
            std::meta::define_aggregate(^^Type, members);
        }
    };
    inline UA_String ua_string(std::string_view value)
    { return { value.size(), reinterpret_cast<UA_Byte*>(const_cast<char*>(value.data())) }; }
    inline UA_NodeId ua_id(UA_UInt16 ns, std::string_view value)
    { UA_NodeId id{}; id.namespaceIndex = ns; id.identifierType = UA_NODEIDTYPE_STRING; id.identifier.string = ua_string(value); return id; }
    inline UA_LocalizedText ua_text(std::string_view value)
    { return { ua_string("en"), ua_string(value) }; }
    class StatusError : public std::exception {
      public:
        explicit StatusError(UA_StatusCode value) : code{value} {}
        const char* what() const noexcept override {return UA_StatusCode_name(code);}
        UA_StatusCode code;
    };
    inline void check(UA_StatusCode code)
    { if (code) throw StatusError(code); }

    // Separate generated UA layouts are populated field by field. Native
    // application records, pointers and std::array layouts never go on the wire.
    class Types
    {
      public:
        Types(UA_Server* server, UA_UInt16 ns) : m_server{ server }, m_namespace{ ns } {}
        template<class T> const UA_DataType* type()
        {
            if constexpr (std::same_as<T, bool>) return &UA_TYPES[UA_TYPES_BOOLEAN];
            else if constexpr (std::same_as<T, double>) return &UA_TYPES[UA_TYPES_DOUBLE];
            else if constexpr (std::same_as<T, std::uint64_t>) return &UA_TYPES[UA_TYPES_UINT64];
            else if constexpr (std::same_as<T, std::uint32_t>) return &UA_TYPES[UA_TYPES_UINT32];
            else if constexpr (std::same_as<T, std::int32_t> || std::is_enum_v<T>) return &UA_TYPES[UA_TYPES_INT32];
            else if constexpr (Array<T>::value) {
                if constexpr (std::same_as<typename Array<T>::Element, char>) return &UA_TYPES[UA_TYPES_STRING];
                else return type<typename Array<T>::Element>();
            } else return record<T>();
        }
        template<class T> UA_StatusCode encode(const T& from, typename Wire<T>::Type& to)
        {
            if constexpr (std::is_enum_v<T>) to = static_cast<UA_Int32>(from);
            else if constexpr (std::is_arithmetic_v<T>) to = from;
            else if constexpr (Array<T>::value) {
                using Element = typename Array<T>::Element;
                if constexpr (std::same_as<Element, char>) {
                    const auto end{ std::ranges::find(from, '\0') };
                    const auto text{ ua_string({ from.data(), static_cast<std::size_t>(end - from.begin()) }) };
                    return UA_String_copy(&text, &to);
                } else {
                    to.data = static_cast<typename Wire<Element>::Type*>(UA_Array_new(from.size(), type<Element>()));
                    if (!to.data) return UA_STATUSCODE_BADOUTOFMEMORY;
                    to.size = from.size();
                    for (unsigned i{}; i < from.size(); ++i) if (auto code{ encode(from[i], to.data[i]) }; code) return code;
                }
            } else {
                static constexpr auto source{ fields<T>() };
                static constexpr auto target{ fields<typename Wire<T>::Type>() };
                template for (constexpr auto i : std::views::iota(0UZ, source.size())) {
                    if (auto code{ encode(from.[:source[i]:], to.[:target[i]:]) }; code) return code;
                }
            }
            return UA_STATUSCODE_GOOD;
        }
        template<class T> UA_StatusCode variant(const T& value, UA_Variant& output)
        {
            if constexpr (Array<T>::value) {
                if constexpr (!std::same_as<typename Array<T>::Element, char>) {
                    typename Wire<T>::Type wire{};
                    const auto code{encode(value, wire)};
                    if (code) {UA_Array_delete(wire.data, wire.size, type<T>()); return code;}
                    UA_Variant_setArray(&output, wire.data, wire.size, type<T>());
                    return UA_STATUSCODE_GOOD;
                }
            }
            const auto* descriptor{ type<T>() };
            auto* wire{ static_cast<typename Wire<T>::Type*>(UA_new(descriptor)) };
            if (!wire) return UA_STATUSCODE_BADOUTOFMEMORY;
            const auto code{ encode(value, *wire) };
            if (code) { UA_delete(wire, descriptor); return code; }
            UA_Variant_setScalar(&output, wire, descriptor);
            return UA_STATUSCODE_GOOD;
        }
        template<class T> UA_StatusCode decode(const typename Wire<T>::Type& from, T& to)
        {
            if constexpr (std::is_enum_v<T>) {
                constexpr auto values{ pnm::meta::enumeration::underlying_enumerators<T>() };
                if (std::ranges::find(values, from) == values.end()) return UA_STATUSCODE_BADOUTOFRANGE;
                to = static_cast<T>(from);
            } else if constexpr (std::is_arithmetic_v<T>) {
                if constexpr (std::is_floating_point_v<T>) if (!std::isfinite(from)) return UA_STATUSCODE_BADOUTOFRANGE;
                to = from;
            } else if constexpr (Array<T>::value) {
                using Element = typename Array<T>::Element;
                if constexpr (std::same_as<Element, char>) {
                    if (from.length >= to.size() || (from.length && !from.data)) return UA_STATUSCODE_BADOUTOFRANGE;
                    to.fill(0);
                    for (unsigned i{}; i < from.length; ++i) {
                        if (!from.data[i]) return UA_STATUSCODE_BADINVALIDARGUMENT;
                        to[i] = static_cast<char>(from.data[i]);
                    }
                } else {
                    if (from.size != to.size() || !from.data) return UA_STATUSCODE_BADOUTOFRANGE;
                    for (unsigned i{}; i < to.size(); ++i) if (auto code{ decode(from.data[i], to[i]) }; code) return code;
                }
            } else {
                static constexpr auto source{ fields<typename Wire<T>::Type>() };
                static constexpr auto target{ fields<T>() };
                template for (constexpr auto i : std::views::iota(0UZ, target.size())) {
                    if (auto code{ decode(from.[:source[i]:], to.[:target[i]:]) }; code) return code;
                    if constexpr (constexpr auto bounds = annotation<Bounds>(target[i]); bounds) {
                        const auto valid{[&](auto value) {return value >= bounds->minimum && value <= bounds->maximum;}};
                        if constexpr (Array<std::remove_cvref_t<decltype(to.[:target[i]:])>>::value) {
                            if (!std::ranges::all_of(to.[:target[i]:], valid)) return UA_STATUSCODE_BADOUTOFRANGE;
                        } else if (!valid(to.[:target[i]:])) return UA_STATUSCODE_BADOUTOFRANGE;
                    }
                }
            }
            return UA_STATUSCODE_GOOD;
        }
        template<class T> UA_StatusCode extract(const UA_Variant& input, T& result)
        {
            const auto* descriptor{ type<T>() };
            if constexpr (Array<T>::value) {
                if constexpr (!std::same_as<typename Array<T>::Element, char>) {
                    if (!UA_Variant_hasArrayType(&input, descriptor)) return UA_STATUSCODE_BADTYPEMISMATCH;
                    typename Wire<T>::Type array{input.arrayLength, static_cast<typename Wire<typename Array<T>::Element>::Type*>(input.data)};
                    return decode(array, result);
                }
            }
            if (!UA_Variant_hasScalarType(&input, descriptor)) return UA_STATUSCODE_BADTYPEMISMATCH;
            return decode(*static_cast<const typename Wire<T>::Type*>(input.data), result);
        }
      private:
        template<class T> const UA_DataType* record()
        {
            constexpr auto annotationValue{ annotation<Record>(^^T) };
            static_assert(annotationValue.has_value(), "Unsupported unannotated OPC UA record");
            const auto idText{ path("Types", annotationValue->id) };
            const auto id{ ua_id(m_namespace, idText.data()) };
            if (const auto* existing{ UA_Server_findDataType(m_server, &id) }) return existing;
            using Representation = typename Wire<T>::Type;
            static_assert(sizeof(Representation) < 65536);
            static constexpr auto native{ fields<T>() }, wire{ fields<Representation>() };
            static_assert(native.size() < 256);
            std::array<UA_DataTypeMember, native.size()> members{};
            std::size_t preceding{};
            template for (constexpr auto i : std::views::iota(0UZ, native.size())) {
                using Value = typename[:std::meta::type_of(native[i]):];
                constexpr auto offset{ std::meta::offset_of(wire[i]).bytes };
                auto& member{ members[i] };
                member.memberName = std::meta::identifier_of(native[i]).data();
                member.memberType = type<Value>();
                if (offset - preceding >= 64) throw std::logic_error("Unsupported UA member alignment");
                member.padding = offset - preceding;
                if constexpr (Array<Value>::value) member.isArray = !std::same_as<typename Array<Value>::Element, char>;
                preceding = offset + sizeof(typename Wire<Value>::Type);
            }
            const auto binaryText{ path(idText.data(), "DefaultBinary") };
            UA_DataType descriptor{};
            descriptor.typeName = std::define_static_string(std::string_view(annotationValue->id));
            descriptor.typeId = id;
            descriptor.binaryEncodingId = ua_id(m_namespace, binaryText.data());
            descriptor.memSize = sizeof(Representation); descriptor.typeKind = UA_DATATYPEKIND_STRUCTURE;
            descriptor.membersSize = members.size(); descriptor.members = members.data();
            check(UA_Server_addDataType(m_server, UA_NODEID_NUMERIC(0, UA_NS0ID_STRUCTURE), &descriptor));
            auto attributes{ UA_DataTypeAttributes_default };
            attributes.displayName = ua_text(annotationValue->id);
            check(UA_Server_addDataTypeNode(m_server, id, UA_NODEID_NUMERIC(0, UA_NS0ID_STRUCTURE),
                UA_NODEID_NUMERIC(0, UA_NS0ID_HASSUBTYPE), {m_namespace, ua_string(annotationValue->id)}, attributes, nullptr, nullptr));
            auto encoding{ UA_ObjectAttributes_default };
            encoding.displayName = ua_text("Default Binary");
            check(UA_Server_addObjectNode(m_server, descriptor.binaryEncodingId, UA_NODEID_NULL,
                UA_NODEID_NULL, {0, ua_string("Default Binary")},
                UA_NODEID_NUMERIC(0, UA_NS0ID_DATATYPEENCODINGTYPE), encoding, nullptr, nullptr));
            UA_ExpandedNodeId encoded{}; encoded.nodeId = descriptor.binaryEncodingId;
            check(UA_Server_addReference(m_server, id, UA_NODEID_NUMERIC(0, UA_NS0ID_HASENCODING), encoded, true));
            return UA_Server_findDataType(m_server, &id);
        }
        UA_Server* m_server;
        UA_UInt16 m_namespace;
    };
}
