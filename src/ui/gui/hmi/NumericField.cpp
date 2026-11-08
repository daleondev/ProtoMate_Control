#include "NumericField.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
namespace ui::gui::hmi
{
    NumericField NumericField::forModel(const Model& m)
    {
        NumericField r;
        const auto field = m.field;
        const bool motor = m.page == Page::Motors;
        const bool factors = m.page == Page::Robot || m.page == Page::Cartesian || m.page == Page::Preview;
        if (field < 3) {
            const bool axis = m.page == Page::Axes;
            const bool joints = (m.page == Page::Robot || m.page == Page::Preview) && m.coordinates == 0;
            const unsigned index = axis ? m.axis : field;
            r.name = motor                           ? "Shaft position / distance [degrees]"
                     : (axis || joints) && index < 2 ? "Axis position / distance [degrees]"
                                                     : "Position / distance [mm]";
            const bool absolute = m.absolute || m.page == Page::Preview;
            if ((axis || joints) && m.state.configured && (absolute || m.state.fresh)) {
                const auto& a = m.state.axes[index];
                const auto origin = absolute ? 0. : a.commanded;
                r.minimum = a.minimum - origin;
                r.maximum = a.maximum - origin;
            }
            return r;
        }
        r.minimum = 0;
        switch (field) {
            case 3:
                r.name = motor ? "Speed [rpm]" : m.axis == 2 ? "Speed [mm/s]" : "Speed [degrees/s]";
                r.minimumExclusive = motor;
                if (!motor && m.state.configured && m.state.axes[m.axis].maxSpeed > 0)
                    r.maximum = m.state.axes[m.axis].maxSpeed;
                if (motor && m.motorTab == 1)
                    r.minimum = m.values[9]; // seek must exceed latch
                break;
            case 4:
            case 5:
            case 6:
                r.name = field == 4 ? "Acceleration" : field == 5 ? "Deceleration" : "Jerk";
                if (factors) {
                    r.maximum = 1;
                    r.name = field == 4   ? "Acceleration factor"
                             : field == 5 ? "Deceleration factor"
                                          : "Jerk factor";
                }
                // Saving defaults cannot itself select a default acceleration.
                r.minimumExclusive = motor && m.motorTab == 2 && field != 6;
                break;
            case 7:
                r.name = "Timeout [seconds]";
                r.maximum = motor ? 1e8 : 90;
                r.minimumExclusive = motor && m.motorTab == 1;
                break;
            case 8:
                r.name = "Jog duration [seconds]";
                r.minimumExclusive = true;
                r.maximum = 1e6;
                break;
            case 9:
                r.name = "Latch speed [rpm]";
                r.minimumExclusive = r.maximumExclusive = true;
                r.maximum = m.values[3];
                break;
            case 10:
                r.name = "Speed factor";
                r.minimumExclusive = true;
                r.maximum = 1;
                break;
            case 11:
                r.name = "Brightness [%]";
                r.maximum = 100;
                break;
            default:
                break;
        }
        return r;
    }
    std::optional<double> NumericField::parse(const char* text) const
    {
        // Decimal notation only: no whitespace, exponents, NaN or infinity.
        const char* p = text;
        if (*p == '-' && signedValue())
            ++p;
        bool digit{}, dot{};
        for (; *p; ++p) {
            if (*p >= '0' && *p <= '9')
                digit = true;
            else if (*p == '.' && !dot)
                dot = true;
            else
                return {};
        }
        if (!digit)
            return {};
        char* end{};
        const auto value = std::strtod(text, &end);
        if (*end || !std::isfinite(value) || value < minimum || value > maximum ||
            (minimumExclusive && value == minimum) || (maximumExclusive && value == maximum))
            return {};
        return value;
    }
    void NumericField::describe(std::span<char> text) const
    {
        const auto unbounded = std::numeric_limits<double>::max();
        if (minimum == -unbounded && maximum == unbounded)
            std::snprintf(text.data(), text.size(), "Signed decimal. Apply changes the form only.");
        else if (maximum == unbounded)
            std::snprintf(text.data(),
                          text.size(),
                          "Value %s %.6g. Apply changes the form only.",
                          minimumExclusive ? ">" : ">=",
                          minimum);
        else
            std::snprintf(text.data(),
                          text.size(),
                          "%.6g %s value %s %.6g. Apply changes the form only.",
                          minimum,
                          minimumExclusive ? "<" : "<=",
                          maximumExclusive ? "<" : "<=",
                          maximum);
    }
}
