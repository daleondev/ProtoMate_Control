#include "NumericField.hpp"
#include "Text.hpp"
#include <cmath>
#include <cstdlib>
namespace ui::gui::hmi
{
    namespace
    {
        constexpr double RAW_TIMEOUT_MAX{ 1e8 }, PROGRAM_TIMEOUT_MAX{ 90 }, JOG_DURATION_MAX{ 1e6 };
        constexpr double BRIGHTNESS_MAX{ 100 };
        const char* position_name(bool motor, bool rotary)
        {
            if (motor) {
                return "Shaft position / distance [degrees]";
            }
            return rotary ? "Axis position / distance [degrees]" : "Position / distance [mm]";
        }
        NumericField position_rules(const Model& m)
        {
            NumericField r{};
            const bool axis{ m.page == Page::Axes };
            const bool joints{ (m.page == Page::Robot || m.page == Page::Preview) && m.coordinates == 0 };
            const auto index{ axis ? m.axis : m.field };
            r.name = position_name(m.page == Page::Motors, (axis || joints) && index < fields::POSITION_3);
            const bool absolute{ m.absolute || m.page == Page::Preview };
            if ((axis || joints) && m.state.configured && (absolute || m.state.fresh)) {
                const auto& a{ m.state.axes[index] };
                const auto origin{ absolute ? 0. : a.commanded };
                r.minimum = a.minimum - origin;
                r.maximum = a.maximum - origin;
            }
            return r;
        }
        NumericField speed_rules(const Model& m)
        {
            NumericField r{ .minimum = 0 };
            if (m.page == Page::Motors) {
                r.name = "Speed [rpm]";
                r.minimum_exclusive = true;
                if (m.motor_tab == 1) {
                    r.minimum = m.values[fields::LATCH_SPEED];
                }
            }
            else {
                r.name = m.axis == fields::POSITION_3 ? "Speed [mm/s]" : "Speed [degrees/s]";
                if (m.state.configured && m.state.axes[m.axis].max_speed > 0) {
                    r.maximum = m.state.axes[m.axis].max_speed;
                }
            }
            return r;
        }
        NumericField dynamics_rules(const Model& m)
        {
            constexpr std::array names{ "Acceleration", "Deceleration", "Jerk" };
            constexpr std::array factor_names{ "Acceleration factor", "Deceleration factor", "Jerk factor" };
            const bool factors{ m.page == Page::Robot || m.page == Page::Cartesian ||
                                m.page == Page::Preview };
            NumericField r{ .name = names[m.field - fields::ACCELERATION], .minimum = 0 };
            if (factors) {
                r.maximum = 1;
                r.name = factor_names[m.field - fields::ACCELERATION];
            }
            // Saving defaults cannot itself select a default acceleration.
            r.minimum_exclusive = m.page == Page::Motors && m.motor_tab == 2 && m.field != fields::JERK;
            return r;
        }
    }
    NumericField NumericField::forModel(const Model& m)
    {
        if (m.field < fields::SPEED) {
            return position_rules(m);
        }
        const bool motor{ m.page == Page::Motors };
        switch (m.field) {
            case fields::SPEED:
                return speed_rules(m);
            case fields::ACCELERATION:
            case fields::DECELERATION:
            case fields::JERK:
                return dynamics_rules(m);
            case fields::TIMEOUT:
                return { .name = "Timeout [seconds]",
                         .minimum = 0,
                         .maximum = motor ? RAW_TIMEOUT_MAX : PROGRAM_TIMEOUT_MAX,
                         .minimum_exclusive = motor && m.motor_tab == 1 };
            case fields::DURATION:
                return { .name = "Jog duration [seconds]",
                         .minimum = 0,
                         .maximum = JOG_DURATION_MAX,
                         .minimum_exclusive = true };
            case fields::LATCH_SPEED:
                return { .name = "Latch speed [rpm]",
                         .minimum = 0,
                         .maximum = m.values[fields::SPEED],
                         .minimum_exclusive = true,
                         .maximum_exclusive = true };
            case fields::SPEED_FACTOR:
                return { .name = "Speed factor", .minimum = 0, .maximum = 1, .minimum_exclusive = true };
            case fields::BRIGHTNESS:
                return { .name = "Brightness [%]", .minimum = 0, .maximum = BRIGHTNESS_MAX };
            default:
                return { .minimum = 0 };
        }
    }
    std::optional<double> NumericField::parse(const char* text) const
    {
        // Decimal notation only: no whitespace, exponents, NaN or infinity.
        const char* p{ text };
        if (*p == '-' && signedValue()) {
            ++p;
        }
        bool digit{};
        bool dot{};
        for (; *p; ++p) {
            if (*p >= '0' && *p <= '9') {
                digit = true;
            }
            else if (*p == '.' && !dot) {
                dot = true;
            }
            else {
                return {};
            }
        }
        if (!digit) {
            return {};
        }
        char* end{};
        const auto value{ std::strtod(text, &end) };
        if (*end || !std::isfinite(value) || value < minimum || value > maximum ||
            (minimum_exclusive && value == minimum) || (maximum_exclusive && value == maximum)) {
            return {};
        }
        return value;
    }
    void NumericField::describe(std::span<char> text) const
    {
        const auto unbounded{ std::numeric_limits<double>::max() };
        if (minimum == -unbounded && maximum == unbounded) {
            format(text, "Signed decimal. Apply changes the form only.");
        }
        else if (maximum == unbounded) {
            format(
              text, "Value {} {:.6g}. Apply changes the form only.", minimum_exclusive ? ">" : ">=", minimum);
        }
        else {
            format(text,
                   "{:.6g} {} value {} {:.6g}. Apply changes the form only.",
                   minimum,
                   minimum_exclusive ? "<" : "<=",
                   maximum_exclusive ? "<" : "<=",
                   maximum);
        }
    }
}
