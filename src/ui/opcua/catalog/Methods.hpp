#pragma once
#include "Runtime.hpp"
#include <tuple>

namespace ui::opcua::catalog
{
    template<class> struct Signature;
    template<class R, class... A> struct Signature<R(*)(Call&, A...)> {
        using Return = R; using Arguments = std::tuple<A...>;
        static constexpr std::size_t count{sizeof...(A)};
    };
    template<class T> UA_Argument argument(Types& types, std::string_view title)
    {
        UA_Argument result{};
        result.name = ua_string(title); result.dataType = types.type<T>()->typeId;
        result.valueRank = UA_VALUERANK_SCALAR;
        if constexpr (Array<T>::value) {
            if constexpr (!std::same_as<typename Array<T>::Element, char>) {
                static constexpr UA_UInt32 extent{Array<T>::extent};
                result.valueRank = 1; result.arrayDimensionsSize = 1;
                result.arrayDimensions = const_cast<UA_UInt32*>(&extent);
            }
        }
        return result;
    }
    template<std::meta::info Function>
    void Runtime::method(std::string_view id, std::string_view parent, std::string_view name, Method annotation)
    {
        if (m_error) return;
        using FunctionType = decltype(&[:Function:]);
        using Shape = Signature<FunctionType>;
        using Result = typename Shape::Return;
        using Arguments = typename Shape::Arguments;
        static constexpr auto parameters{std::define_static_array(std::meta::parameters_of(Function))};
        static_assert(parameters.size() == Shape::count + 1);
        std::array<UA_Argument, Shape::count> input{};
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            ((input[I] = argument<std::tuple_element_t<I,Arguments>>(m_types, std::meta::identifier_of(parameters[I+1]))), ...);
        }(std::make_index_sequence<Shape::count>{});
        const auto output{argument<Result>(m_types, "result")};
        MethodBinding binding{this, annotation.access, [](Runtime& self, Session& session, std::span<const UA_Variant> input, UA_Variant& output) -> UA_StatusCode {
            if (input.size() != Shape::count) return UA_STATUSCODE_BADARGUMENTSMISSING;
            Arguments arguments{};
            UA_StatusCode code{};
            [&]<std::size_t... I>(std::index_sequence<I...>) {
                ((code = code ? code : self.m_types.extract(input[I], std::get<I>(arguments))), ...);
            }(std::make_index_sequence<Shape::count>{});
            if (code) return code;
            Call call{self.m_services, session};
            const auto result{std::apply([&](auto&... values) {return [:Function:](call, values...);}, arguments)};
            return self.m_types.variant(result, output);
        }};
        installMethod(binding, id, parent, name, annotation, input, output);
    }
}
