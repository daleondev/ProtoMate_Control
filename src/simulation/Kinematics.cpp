#include "Kinematics.hpp"
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <numbers>
#include <stdexcept>

namespace simulation
{
    namespace
    {
        constexpr double RAD{ std::numbers::pi / 180 };
        ui::motion::RotaryAxisConversion rotary(const Profile& p, unsigned axis)
        {
            return ui::motion::RotaryAxisConversion{ { p.ratio[axis],
                                                       p.direction[axis],
                                                       pnm::units::Angle::create(p.shaft_reference[axis] *
                                                                                 RAD),
                                                       pnm::units::Angle::create(p.axis_reference[axis]) } };
        }
    }
    Kinematics::Kinematics(const Profile& p)
      : m_rotary{ rotary(p, 0), rotary(p, 1) }
      , m_linear{ { pnm::units::Distance::create(p.z_mm_per_revolution / 1000),
                    p.ratio[2],
                    p.direction[2],
                    pnm::units::Angle::create(p.shaft_reference[2] * RAD),
                    pnm::units::Distance::create(p.axis_reference[2] / 1000) } }
    {
        p.validate();
    }
    std::array<double, 3> Kinematics::joints(const std::array<double, 3>& shaft) const
    {
        return { m_rotary[0].toAxisPosition(pnm::units::Angle::create(shaft[0] * RAD)).get(),
                 m_rotary[1].toAxisPosition(pnm::units::Angle::create(shaft[1] * RAD)).get(),
                 m_linear.toAxisPosition(pnm::units::Angle::create(shaft[2] * RAD)).get() * 1000 };
    }
    std::array<double, 3> Kinematics::tcp(const Profile& p, const std::array<double, 3>& q)
    {
        const double first{ std::remainder(q[0], 2 * std::numbers::pi) };
        const double second{ first + std::remainder(q[1], 2 * std::numbers::pi) };
        return {
            p.geometry_mm[0] * std::cos(first) + (p.geometry_mm[1] + p.geometry_mm[2]) * std::cos(second),
            p.geometry_mm[0] * std::sin(first) + (p.geometry_mm[1] + p.geometry_mm[2]) * std::sin(second),
            cad::REST_TCP_Z_MM + q[2]
        };
    }
    glm::dmat4 Kinematics::relativeFrame(const Profile& p, unsigned axis, double joint)
    {
        if (!std::isfinite(joint) || axis >= 3)
            throw std::invalid_argument{ "invalid mechanical coordinate" };
        if (axis < 2) {
            const glm::dvec3 origin{
                axis == 0 ? glm::dvec3{ 0, 0, cad::J1_FRAME_Z_MM }
                          : glm::dvec3{ p.geometry_mm[0], 0, cad::J2_FRAME_Z_MM - cad::J1_FRAME_Z_MM }
            };
            return glm::rotate(glm::translate(glm::dmat4{ 1 }, origin),
                               std::remainder(joint, 2 * std::numbers::pi),
                               glm::dvec3{ 0, 0, 1 });
        }
        return glm::translate(glm::dmat4{ 1 },
                              glm::dvec3{ p.geometry_mm[1] + p.geometry_mm[2],
                                          0,
                                          cad::CARRIAGE_FRAME_Z_MM - cad::J2_FRAME_Z_MM + joint });
    }
    std::array<glm::dmat4, 4> Kinematics::frames(const Profile& p, const std::array<double, 3>& q)
    {
        const auto first{ relativeFrame(p, 0, q[0]) };
        const auto second{ first * relativeFrame(p, 1, q[1]) };
        return { glm::dmat4{ 1 }, first, second, second * relativeFrame(p, 2, q[2]) };
    }
}
