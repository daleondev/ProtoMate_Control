#include "Kinematics.hpp"
#include "common/motion/VisualGeometry.hpp"
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <numbers>
#include <stdexcept>

namespace simulation
{
    namespace
    {
        constexpr double MILLIMETRES_PER_METRE{ 1000 };
    }
    namespace
    {
        constexpr double RAD{ std::numbers::pi / 180 };
        ui::motion::RotaryAxisConversion rotary(const Profile& p, unsigned axis)
        {
            return ui::motion::RotaryAxisConversion{
                { .motor_revolutions_per_axis_revolution = p.ratio[axis],
                  .direction = p.direction[axis],
                  .motor_reference = pnm::units::Angle::create(p.shaft_reference[axis] * RAD),
                  .axis_reference = pnm::units::Angle::create(p.axis_reference[axis]) }
            };
        }
    }
    Kinematics::Kinematics(const Profile& p)
      : m_rotary{ rotary(p, 0), rotary(p, 1) }
      , m_linear{ { .travel_per_output_revolution =
                      pnm::units::Distance::create(p.z_mm_per_revolution / MILLIMETRES_PER_METRE),
                    .motor_revolutions_per_output_revolution = p.ratio[2],
                    .direction = p.direction[2],
                    .motor_reference = pnm::units::Angle::create(p.shaft_reference[2] * RAD),
                    .axis_reference =
                      pnm::units::Distance::create(p.axis_reference[2] / MILLIMETRES_PER_METRE) } }
    {
        p.validate();
    }
    std::array<double, 3> Kinematics::joints(const std::array<double, 3>& shaft) const
    {
        return { m_rotary[0].toAxisPosition(pnm::units::Angle::create(shaft[0] * RAD)).get(),
                 m_rotary[1].toAxisPosition(pnm::units::Angle::create(shaft[1] * RAD)).get(),
                 m_linear.toAxisPosition(pnm::units::Angle::create(shaft[2] * RAD)).get() *
                   MILLIMETRES_PER_METRE };
    }
    std::array<double, 3> Kinematics::tcp(const Profile& p, const std::array<double, 3>& q)
    {
        const double first{ std::remainder(q[0], 2 * std::numbers::pi) };
        const double second{ first + std::remainder(q[1], 2 * std::numbers::pi) };
        return {
            (p.geometry_mm[0] * std::cos(first)) + ((p.geometry_mm[1] + p.geometry_mm[2]) * std::cos(second)),
            (p.geometry_mm[0] * std::sin(first)) + ((p.geometry_mm[1] + p.geometry_mm[2]) * std::sin(second)),
            cad::REST_TCP_Z_MM + q[2]
        };
    }
    glm::dmat4 Kinematics::relativeFrame(const Profile& p, unsigned axis, double joint)
    {
        const std::array<double, 3> lengths{ p.geometry_mm[0] / MILLIMETRES_PER_METRE,
                                             p.geometry_mm[1] / MILLIMETRES_PER_METRE,
                                             p.geometry_mm[2] / MILLIMETRES_PER_METRE };
        const auto f{ ::motion::visual::relative(
          axis, axis == 2 ? joint / MILLIMETRES_PER_METRE : joint, lengths) };
        return glm::rotate(glm::translate(glm::dmat4{ 1 },
                                          glm::dvec3{ f.translation[0] * MILLIMETRES_PER_METRE,
                                                      f.translation[1] * MILLIMETRES_PER_METRE,
                                                      f.translation[2] * MILLIMETRES_PER_METRE }),
                           f.yaw,
                           glm::dvec3{ 0, 0, 1 });
    }
    std::array<glm::dmat4, 4> Kinematics::frames(const Profile& p, const std::array<double, 3>& q)
    {
        const auto first{ relativeFrame(p, 0, q[0]) };
        const auto second{ first * relativeFrame(p, 1, q[1]) };
        return { glm::dmat4{ 1 }, first, second, second * relativeFrame(p, 2, q[2]) };
    }
}
