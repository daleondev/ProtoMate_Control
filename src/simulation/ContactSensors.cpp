#include "ContactSensors.hpp"
#include "CollisionMesh.hpp"
#include "Kinematics.hpp"
#include "assets/EmbeddedAssets.hpp"
#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <numbers>
#include <stdexcept>

namespace simulation
{
    namespace
    {
        struct Geometry
        {
            std::array<CollisionMesh, 3> links;
            std::array<CollisionMesh, 3> levers;
            Geometry()
              : links{ CollisionMesh{ assets::collisionLinks()[0] },
                       CollisionMesh{ assets::collisionLinks()[1] },
                       CollisionMesh{ assets::collisionLinks()[2] } }
              , levers{ CollisionMesh{ assets::switchLevers()[0] },
                        CollisionMesh{ assets::switchLevers()[1] },
                        CollisionMesh{ assets::switchLevers()[2] } }
            {
            }
        };
        glm::dmat4 leverTransform(unsigned axis, double deflection)
        {
            const auto coordinates{ cad::LEVER_ROOTS_MM[axis] }, press{ cad::LEVER_PRESS_AXES[axis] };
            const glm::dvec3 root{ coordinates[0], coordinates[1], coordinates[2] };
            return glm::translate(glm::rotate(glm::translate(glm::dmat4{ 1 }, root),
                                              deflection * std::numbers::pi / 180,
                                              glm::dvec3{ press[0], press[1], press[2] }),
                                  -root);
        }
        const Geometry& geometry()
        {
            static const Geometry value{};
            return value;
        }
    }
    ContactSensors::ContactSensors(const Profile& profile)
      : m_profile{ profile }
    {
        m_profile.validate();
        const auto& g{ geometry() };
        for (unsigned axis{}; axis < 2; ++axis) {
            const double origin{ axis == 0 ? 0 : profile.geometry_mm[0] };
            m_radius[axis] =
              std::max(g.levers[axis].radiusFromZAxis(origin, 0, glm::dmat4{ 1 }),
                       g.levers[axis].radiusFromZAxis(
                         origin, 0, leverTransform(axis, profile.sensor_operate_deflection_deg[axis])));
        }
        m_radius[2] = 1; // Z displacement is already in millimetres.
    }
    double ContactSensors::clearance(const Profile& p, unsigned axis, double joint, double deflection)
    {
        if (axis >= 3 || !std::isfinite(deflection) || deflection < 0 || deflection > 15)
            throw std::invalid_argument{ "invalid switch geometry query" };
        const auto flexed{ leverTransform(axis, deflection) };
        const auto local{ glm::inverse(Kinematics::relativeFrame(p, axis, joint)) * flexed };
        const auto& g{ geometry() };
        return g.links[axis].distance(g.levers[axis], local);
    }
    bool ContactSensors::contact(const Profile& p, unsigned axis, double joint, double deflection)
    {
        return clearance(p, axis, joint, 0) == 0 &&
               (deflection == 0 || clearance(p, axis, joint, deflection) == 0);
    }
    ContactSensors::Boundary ContactSensors::boundary(const Profile& p, unsigned axis, bool positive)
    {
        p.validate();
        if (axis >= 3 || (axis == 2 && !positive))
            throw std::invalid_argument{ "no CAD cam on this axis side" };
        const double step = (positive ? 1. : -1.) * (axis < 2 ? std::numbers::pi / 180. : 1.);
        const auto operated = [&](double distance) {
            return contact(p, axis, distance * step, p.sensor_operate_deflection_deg[axis]);
        };
        if (operated(0) || clearance(p, axis, 0, 0) <= p.sensor_release_clearance_mm[axis])
            throw std::runtime_error{ "CAD zero must be clear of both cams" };
        unsigned end = 1;
        for (; end <= 180 && !operated(end); ++end) {
        }
        if (end > 180)
            throw std::runtime_error{ "CAD cam not found within supported travel" };
        const auto bisect = [&](auto predicate) {
            double low = 0, high = end;
            for (unsigned iteration = 0; iteration < 40; ++iteration) {
                const double middle = (low + high) * .5;
                if (predicate(middle))
                    high = middle;
                else
                    low = middle;
            }
            return high * step;
        };
        return { bisect(operated), bisect([&](double distance) {
            return clearance(p, axis, distance * step, 0) <= p.sensor_release_clearance_mm[axis];
        }) };
    }
    bool ContactSensors::update(unsigned axis, double joint)
    {
        if (axis >= 3 || !std::isfinite(joint))
            throw std::invalid_argument{ "invalid home sensor coordinate" };
        const double travel{ std::abs(joint - m_previous[axis]) * m_radius[axis] };
        // Surface distance is Lipschitz under this bounded rigid displacement.
        // This avoids re-running triangle queries for every submicrometre Z step.
        if (joint == m_previous[axis] || travel < std::max(0.0, m_margin[axis] - 1e-7) * .5)
            return m_active[axis];
        m_previous[axis] = joint;
        const double free_gap{ clearance(m_profile, axis, joint, 0) };
        if (m_active[axis]) {
            const double release{ m_profile.sensor_release_clearance_mm[axis] };
            m_active[axis] = free_gap <= release;
            m_margin[axis] = std::abs(free_gap - release);
        }
        else if (free_gap > 0)
            m_margin[axis] = free_gap;
        else {
            const double operated_gap{ clearance(
              m_profile, axis, joint, m_profile.sensor_operate_deflection_deg[axis]) };
            m_active[axis] = operated_gap == 0;
            m_margin[axis] = m_active[axis] ? m_profile.sensor_release_clearance_mm[axis] : operated_gap;
        }
        return m_active[axis];
    }
}
