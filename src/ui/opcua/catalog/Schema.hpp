#pragma once
#include "Annotations.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>

namespace ui::opcua::catalog
{
    using Path = std::array<char, 160>;
    inline Path path(std::string_view parent, std::string_view name)
    {
        Path result{};
        if (parent.size() + name.size() + 2 > result.size()) throw std::logic_error("OPC UA node ID exceeds bound");
        auto next{ std::copy(parent.begin(), parent.end(), result.begin()) };
        *next++ = '/';
        std::copy(name.begin(), name.end(), next);
        return result;
    }
    template<class T> consteval auto methods()
    {
        std::vector<std::meta::info> result;
        for (auto member : std::meta::members_of(^^T, std::meta::access_context::current()))
            if (std::meta::is_function(member) && annotation<Method>(member)) result.push_back(member);
        return std::define_static_array(result);
    }
    template<class T>
    consteval void identities(std::string parent, std::vector<std::string>& result)
    {
        static constexpr auto members{ fields<T>() };
        static_assert(members.size() == pnm::meta::structural::field_count<T>());
        template for (constexpr auto member : members) {
            constexpr auto object{ annotation<Object>(member) };
            constexpr auto variable{ annotation<Variable>(member) };
            static_assert(!(object && variable), "Member cannot be both object and variable");
            if constexpr (object || variable) {
                constexpr auto name{ object ? object->id : variable->id };
                static_assert(!name.empty() && name.find('/') == std::string_view::npos, "Export needs a stable local ID without '/'");
                const auto id{ parent + '/' + std::string(name) };
                if (std::ranges::find(result, id) != result.end()) throw "Duplicate OPC UA node identity";
                result.push_back(id);
                if constexpr (object) identities<typename[:std::meta::type_of(member):]>(id, result);
                else static_assert(variable->access == Access::ReadOnly, "Configuration/motion changes must be methods");
            }
        }
        template for (constexpr auto member : methods<T>()) {
            if constexpr (constexpr auto method = annotation<Method>(member); method) {
                static_assert(std::meta::is_static_member(member), "Catalog methods are stateless adapters");
                static_assert(!method->id.empty() && method->id.find('/') == std::string_view::npos);
                const auto id{parent + '/' + std::string(method->id)};
                if (std::ranges::find(result, id) != result.end()) throw "Duplicate OPC UA node identity";
                result.push_back(id);
            }
        }
    }
    template<class T> consteval bool valid_schema()
    {
        std::vector<std::string> ids;
        identities<T>("ProtoMate", ids);
        return true;
    }
    struct SchemaSize {std::size_t variables{}, methods{};};
    template<class T> consteval SchemaSize schema_size()
    {
        SchemaSize size{.methods = methods<T>().size()};
        template for (constexpr auto member : fields<T>()) {
            if constexpr (annotation<Object>(member)) {
                constexpr auto child{schema_size<typename[:std::meta::type_of(member):]>()};
                size.variables += child.variables; size.methods += child.methods;
            } else if constexpr (annotation<Variable>(member)) ++size.variables;
        }
        return size;
    }
    template<class T, class Visitor>
    void visit(T& value, std::string_view parent, Visitor& visitor)
    {
        static constexpr auto members{ fields<T>() };
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            ([&] {
                constexpr auto member{ members[I] };
                constexpr auto object{ annotation<Object>(member) };
                constexpr auto variable{ annotation<Variable>(member) };
                if constexpr (object || variable) {
                    constexpr auto name{ object ? object->id : variable->id };
                    constexpr auto fallback{ pnm::meta::structural::field_name<I, T>() };
                    const auto id{ path(parent, name) };
                    if constexpr (object) {
                        visitor.object(id.data(), parent, name.empty() ? fallback : name, *object);
                        visit(value.[:member:], id.data(), visitor);
                    } else visitor.variable(value.[:member:], id.data(), parent, name.empty() ? fallback : name, *variable);
                }
            }(), ...);
        }(std::make_index_sequence<members.size()>{});
        template for (constexpr auto member : methods<T>()) {
            if constexpr (constexpr auto method = annotation<Method>(member); method) {
                const auto id{path(parent, method->id)};
                visitor.template method<member>(id.data(), parent, method->id, *method);
            }
        }
    }
}
