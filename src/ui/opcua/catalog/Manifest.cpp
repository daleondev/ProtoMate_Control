#include "Model.hpp"
#include "Methods.hpp"
#include <iostream>
#include <iomanip>
#include <set>
namespace c = ui::opcua::catalog;
namespace {
    class Manifest {
      public:
        void object(std::string_view id, std::string_view, std::string_view, c::Object a) {
            separator(); std::cout << "{\"id\":" << std::quoted(std::string(id)) << ",\"kind\":\"object\",\"description\":" << std::quoted(std::string(a.description)) << "}";
        }
        template<class T> void variable(T&, std::string_view id, std::string_view, std::string_view, c::Variable a) {
            separator(); std::cout << "{\"id\":" << std::quoted(std::string(id)) << ",\"kind\":\"variable\",\"access\":\"read\",\"value\":";
            type<T>(); std::cout << ",\"unit\":" << std::quoted(std::string(c::unit_name(a.unit))) << ",\"description\":" << std::quoted(std::string(a.description)) << "}";
        }
        template<std::meta::info Function> void method(std::string_view id, std::string_view, std::string_view, c::Method a) {
            using Shape = c::Signature<decltype(&[:Function:])>;
            static constexpr auto parameters{std::define_static_array(std::meta::parameters_of(Function))};
            separator(); std::cout << "{\"id\":" << std::quoted(std::string(id)) << ",\"kind\":\"method\",\"access\":" << std::quoted(a.access == c::Access::ReadOnly ? "read" : a.access == c::Access::Cleanup ? "cleanup" : "command")
                << ",\"description\":" << std::quoted(std::string(a.description)) << ",\"inputs\":[";
            [&]<std::size_t... I>(std::index_sequence<I...>) {
                ([&] {
                    if constexpr (I) std::cout << ',';
                    std::cout << "{\"name\":" << std::quoted(std::string(std::meta::identifier_of(parameters[I+1]))) << ",\"value\":";
                    type<std::tuple_element_t<I,typename Shape::Arguments>>(); std::cout << "}";
                }(), ...);
            }(std::make_index_sequence<Shape::count>{});
            std::cout << "],\"output\":"; type<typename Shape::Return>(); std::cout << "}";
        }
      private:
        bool first{true};
        void separator() {if (!first) std::cout << ','; first = false; std::cout << '\n';}
        template<class T> void type() {
            if constexpr (c::Array<T>::value) {
                if constexpr (std::same_as<typename c::Array<T>::Element,char>) std::cout << "{\"type\":\"String\",\"capacity\":" << c::Array<T>::extent-1 << "}";
                else {std::cout << "{\"array\":" << c::Array<T>::extent << ",\"element\":"; type<typename c::Array<T>::Element>(); std::cout << "}";}
            } else if constexpr (std::is_enum_v<T>) {
                std::cout << "{\"type\":\"Int32\",\"values\":[";
                constexpr auto names{pnm::meta::enumeration::enumerator_names<T>()};
                constexpr auto values{pnm::meta::enumeration::underlying_enumerators<T>()};
                for (unsigned i{}; i < names.size(); ++i) {if (i) std::cout << ','; std::cout << "{\"name\":" << std::quoted(std::string(names[i])) << ",\"value\":" << +values[i] << "}";}
                std::cout << "]}";
            } else if constexpr (std::is_arithmetic_v<T>) {
                constexpr auto name{std::same_as<T,bool> ? "Boolean" : std::same_as<T,double> ? "Double" : std::same_as<T,std::uint64_t> ? "UInt64" : std::same_as<T,std::uint32_t> ? "UInt32" : "Int32"};
                std::cout << "{\"type\":" << std::quoted(name) << "}";
            } else {
                constexpr auto record{c::annotation<c::Record>(^^T)}; static_assert(record.has_value());
                std::cout << "{\"type\":" << std::quoted("Types/" + std::string(record->id)) << ",\"fields\":[";
                bool firstField{true};
                template for (constexpr auto field : c::fields<T>()) {
                    constexpr auto a{c::annotation<c::Field>(field)}; static_assert(a.has_value());
                    if (!firstField) std::cout << ','; firstField = false;
                    std::cout << "{\"name\":" << std::quoted(std::string(std::meta::identifier_of(field))) << ",\"value\":";
                    type<typename[:std::meta::type_of(field):]>();
                    std::cout << ",\"unit\":" << std::quoted(std::string(c::unit_name(a->unit))) << ",\"description\":" << std::quoted(std::string(a->description));
                    if constexpr (constexpr auto bounds = c::annotation<c::Bounds>(field); bounds) std::cout << ",\"minimum\":" << bounds->minimum << ",\"maximum\":" << bounds->maximum;
                    std::cout << "}";
                }
                std::cout << "]}";
            }
        }
    };
}
int main() {
    static_assert(c::valid_schema<c::Model>());
    c::Model model{}; Manifest visitor;
    std::cout << "{\"namespace\":\"urn:protomate:control\",\"version\":\"1.0.0\",\"nodes\":[";
    c::visit(model, "ProtoMate", visitor);
    std::cout << "\n]}\n";
}
