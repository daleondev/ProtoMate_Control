#pragma once
#include <meta>
#include <optional>
#include <string_view>
#include <pneumo/meta.hpp>

namespace ui::opcua::catalog
{
    // Annotations require structural types. Own the literal bytes instead of
    // embedding std::string_view, which has private non-structural members.
    template<std::size_t N> struct Literal
    {
        std::array<char, N> bytes{};
        std::size_t length{};
        constexpr Literal() = default;
        template<std::size_t M> consteval Literal(const char (&text)[M]) : length{ M - 1 }
        { static_assert(M <= N); std::copy_n(text, M, bytes.begin()); }
        constexpr operator std::string_view() const { return { bytes.data(), length }; }
        constexpr bool empty() const { return length == 0; }
        constexpr auto find(char c) const { return std::string_view(*this).find(c); }
    };
    using Identity = Literal<64>;
    using Description = Literal<192>;
    enum class Quality { Local, Telemetry, Configured, RobotConfigured,
        Axis1Feedback, Axis2Feedback, Axis3Feedback, RobotFeedback,
        Motor1Feedback, Motor2Feedback, Motor3Feedback };
    enum class Unit { None, Degrees, Millimetres, Rpm, DegreesPerSecond, MillimetresPerSecond, Milliseconds, Bytes };
    enum class Access { ReadOnly, Command, Cleanup };
    struct Object { Identity id; Description description{}; };
    struct Variable
    {
        Identity id;
        Description description{};
        Unit unit{};
        Quality quality{ Quality::Telemetry };
        Access access{ Access::ReadOnly };
    };
    struct Method { Identity id; Description description; Access access{ Access::Command }; };
    struct Record { Identity id; bool input{}; };
    struct Field { Description description{}; Unit unit{}; };
    struct Bounds { double minimum{}, maximum{}; };

    template<class A>
    consteval std::optional<A> annotation(std::meta::info member)
    {
        std::optional<A> found;
        for (auto value : std::meta::annotations_of(member)) {
            if (std::meta::is_same_type(std::meta::remove_const(std::meta::type_of(value)), ^^A)) {
                if (found) throw "Duplicate OPC UA annotation";
                found = std::meta::extract<A>(value);
            }
        }
        return found;
    }
    template<class T>
    consteval auto fields()
    {
        return std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()));
    }
    constexpr std::string_view unit_name(Unit unit)
    {
        switch (unit) {
            case Unit::Degrees: return "deg";
            case Unit::Millimetres: return "mm";
            case Unit::Rpm: return "rpm";
            case Unit::DegreesPerSecond: return "deg/s";
            case Unit::MillimetresPerSecond: return "mm/s";
            case Unit::Milliseconds: return "ms";
            case Unit::Bytes: return "B";
            default: return "";
        }
    }
}
