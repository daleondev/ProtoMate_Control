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
        constexpr double DEGREE_RADIANS{ std::numbers::pi / 180. };
        constexpr double MAX_LEVER_DEFLECTION_DEG{ 15. };
        constexpr unsigned CAM_SEARCH_STEPS{ 180 };
        constexpr unsigned BISECTION_ITERATIONS{ 40 };
        constexpr double CLEARANCE_EPSILON_MM{ 1e-7 };
        constexpr double TRAVEL_MARGIN_FACTOR{ .5 };
        struct Geometry
        {
            std::array<CollisionMesh, 3> links;
            std::array<CollisionMesh, 3> levers;
            Geometry()
              : links{ CollisionMesh{ assets::collision_links()[0] },
                       CollisionMesh{ assets::collision_links()[1] },
                       CollisionMesh{ assets::collision_links()[2] } }
              , levers{ CollisionMesh{ assets::switch_levers()[0] },
                        CollisionMesh{ assets::switch_levers()[1] },
                        CollisionMesh{ assets::switch_levers()[2] } }
            {
            }
        };
        glm::dmat4 lever_transform(const ContactSensors::Query& query)
        {
            const auto coordinates{ cad::LEVER_ROOTS_MM[query.axis] };
            const auto press{ cad::LEVER_PRESS_AXES[query.axis] };
            const glm::dvec3 root{ coordinates[0], coordinates[1], coordinates[2] };
            return glm::translate(glm::rotate(glm::translate(glm::dmat4{ 1 }, root),
                                              query.deflection * DEGREE_RADIANS,
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
                         origin,
                         0,
                         lever_transform(
                           { .axis = axis, .deflection = profile.sensor_operate_deflection_deg[axis] })));
        }
        m_radius[2] = 1; // Z displacement is already in millimetres.
    }
    double ContactSensors::clearance(const Profile& p, Query query)
    {
        const auto [axis, joint, deflection]{ query };
        if (axis >= 3 || !std::isfinite(deflection) || deflection < 0 ||
            deflection > MAX_LEVER_DEFLECTION_DEG) {
            throw std::invalid_argument{ "invalid switch geometry query" };
        }
        const auto flexed{ lever_transform(query) };
        const auto local{ glm::inverse(Kinematics::relativeFrame(p, axis, joint)) * flexed };
        const auto& g{ geometry() };
        return g.links[axis].distance(g.levers[axis], local);
    }
    bool ContactSensors::contact(const Profile& p, Query query)
    {
        const auto [axis, joint, deflection]{ query };
        return clearance(p, { .axis = axis, .joint = joint, .deflection = 0 }) == 0 &&
               (deflection == 0 ||
                clearance(p, { .axis = axis, .joint = joint, .deflection = deflection }) == 0);
    }
    ContactSensors::Boundary ContactSensors::boundary(const Profile& p, unsigned axis, bool positive)
    {
        p.validate();
        if (axis >= 3 || (axis == 2 && !positive)) {
            throw std::invalid_argument{ "no CAD cam on this axis side" };
        }
        const double step{ (positive ? 1. : -1.) * (axis < 2 ? DEGREE_RADIANS : 1.) };
        const auto operated{ [&p, axis, step](double distance) {
            return contact(p,
                           { .axis = axis,
                             .joint = distance * step,
                             .deflection = p.sensor_operate_deflection_deg[axis] });
        } };
        if (operated(0) || clearance(p, { .axis = axis, .joint = 0, .deflection = 0 }) <=
                             p.sensor_release_clearance_mm[axis]) {
            throw std::runtime_error{ "CAD zero must be clear of both cams" };
        }
        unsigned end{ 1 };
        for (; end <= CAM_SEARCH_STEPS && !operated(end); ++end) {
        }
        if (end > CAM_SEARCH_STEPS) {
            throw std::runtime_error{ "CAD cam not found within supported travel" };
        }
        const auto bisect{ [&](auto predicate) {
            double low{ 0 };
            double high{ static_cast<double>(end) };
            for (unsigned iteration{ 0 }; iteration < BISECTION_ITERATIONS; ++iteration) {
                const double middle{ (low + high) * .5 };
                if (predicate(middle)) {
                    high = middle;
                }
                else {
                    low = middle;
                }
            }
            return high * step;
        } };
        return { .operate = bisect(operated), .release = bisect([&](double distance) {
            return clearance(p, { .axis = axis, .joint = distance * step, .deflection = 0 }) <=
                   p.sensor_release_clearance_mm[axis];
        }) };
    }
    bool ContactSensors::update(unsigned axis, double joint)
    {
        if (axis >= 3 || !std::isfinite(joint)) {
            throw std::invalid_argument{ "invalid home sensor coordinate" };
        }
        const double travel{ std::abs(joint - m_previous[axis]) * m_radius[axis] };
        // Surface distance is Lipschitz under this bounded rigid displacement.
        // This avoids re-running triangle queries for every submicrometre Z step.
        if (joint == m_previous[axis] ||
            travel < std::max(0.0, m_margin[axis] - CLEARANCE_EPSILON_MM) * TRAVEL_MARGIN_FACTOR) {
            return m_active[axis];
        }
        m_previous[axis] = joint;
        const double free_gap{ clearance(m_profile, { .axis = axis, .joint = joint, .deflection = 0 }) };
        if (m_active[axis]) {
            const double release{ m_profile.sensor_release_clearance_mm[axis] };
            m_active[axis] = free_gap <= release;
            m_margin[axis] = std::abs(free_gap - release);
        }
        else if (free_gap > 0) {
            m_margin[axis] = free_gap;
        }
        else {
            const double operated_gap{ clearance(
              m_profile,
              { .axis = axis,
                .joint = joint,
                .deflection = m_profile.sensor_operate_deflection_deg[axis] }) };
            m_active[axis] = operated_gap == 0;
            m_margin[axis] = m_active[axis] ? m_profile.sensor_release_clearance_mm[axis] : operated_gap;
        }
        return m_active[axis];
    }
}
