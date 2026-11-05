#include "../CollisionMesh.hpp"
#include "../ContactSensors.hpp"
#include "../Kinematics.hpp"
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string_view>

namespace
{
    std::vector<simulation::CollisionMesh::Triangle> box(double radius)
    {
        const std::array<glm::dvec3, 8> v{ { { -radius, -radius, -radius },
                                             { radius, -radius, -radius },
                                             { radius, radius, -radius },
                                             { -radius, radius, -radius },
                                             { -radius, -radius, radius },
                                             { radius, -radius, radius },
                                             { radius, radius, radius },
                                             { -radius, radius, radius } } };
        constexpr std::array<std::array<unsigned, 3>, 12> faces{ { { 0, 2, 1 },
                                                                   { 0, 3, 2 },
                                                                   { 4, 5, 6 },
                                                                   { 4, 6, 7 },
                                                                   { 0, 1, 5 },
                                                                   { 0, 5, 4 },
                                                                   { 1, 2, 6 },
                                                                   { 1, 6, 5 },
                                                                   { 2, 3, 7 },
                                                                   { 2, 7, 6 },
                                                                   { 3, 0, 4 },
                                                                   { 3, 4, 7 } } };
        std::vector<simulation::CollisionMesh::Triangle> result{};
        for (auto f : faces)
            result.push_back({ v[f[0]], v[f[1]], v[f[2]] });
        return result;
    }
    TEST(CollisionMeshTest, ClosedSolidsAndInvalidGeometry)
    {
        using simulation::CollisionMesh;
        const CollisionMesh cube{ box(1) }, large{ box(4) };
        ASSERT_TRUE((cube.contains({ 0, 0, 0 }) && !cube.contains({ 2, 0, 0 }))) << "solid containment";
        ASSERT_TRUE((large.intersects(cube, glm::dmat4{ 1 }) && cube.intersects(large, glm::dmat4{ 1 })))
          << "nested closed solids without surface crossings";
        ASSERT_TRUE((cube.intersects(cube, glm::translate(glm::dmat4{ 1 }, glm::dvec3{ 2, 0, 0 }))))
          << "coplanar face contact";
        ASSERT_TRUE(
          (std::abs(cube.distance(cube, glm::translate(glm::dmat4{ 1 }, glm::dvec3{ 2.125, 0, 0 })) - .125) <
           1e-12))
          << "exact face clearance";
        ASSERT_TRUE((!cube.intersects(cube, glm::translate(glm::dmat4{ 1 }, glm::dvec3{ 2.01, 0, 0 }))))
          << "separated coplanar faces";
        ASSERT_TRUE((cube.intersects(
          cube,
          glm::rotate(glm::translate(glm::dmat4{ 1 }, glm::dvec3{ 1.8, 0, 0 }), .4, glm::dvec3{ 0, 0, 1 }))))
          << "rotated edge collision";
        const glm::dvec3 a{ 0, 0, 0 }, b{ 1, 0, 0 }, c{ 0, 1, 0 }, d{ 0, 0, 1 };
        const CollisionMesh tetrahedron{ std::vector<CollisionMesh::Triangle>{
          { a, b, c }, { a, d, b }, { a, c, d }, { b, d, c } } };
        ASSERT_TRUE(
          (!tetrahedron.intersects(tetrahedron, glm::translate(glm::dmat4{ 1 }, glm::dvec3{ .6, .6, .6 }))))
          << "overlapping AABBs are not a collision";
        auto compound{ box(4) };
        for (auto t : box(4)) {
            const glm::dvec3 offset{ 20, 0, 0 };
            t.a += offset;
            t.b += offset;
            t.c += offset;
            compound.push_back(t);
        }
        const CollisionMesh disconnected{ compound };
        ASSERT_TRUE((disconnected.intersects(cube, glm::translate(glm::dmat4{ 1 }, glm::dvec3{ 20, 0, 0 }))))
          << "containment in disconnected component";
        ASSERT_TRUE((cube.intersects(disconnected, glm::translate(glm::dmat4{ 1 }, glm::dvec3{ -20, 0, 0 }))))
          << "reverse containment in disconnected component";
        auto reversed{ box(1) };
        for (auto& t : reversed)
            std::swap(t.b, t.c);
        ASSERT_TRUE((CollisionMesh{ reversed }.contains({ 0, 0, 0 }))) << "winding-independent containment";
        auto open{ box(1) };
        open.pop_back();
        EXPECT_THROW(([&] { CollisionMesh bad{ open }; })(), std::exception) << "open mesh rejection";
        auto invalid{ box(1) };
        invalid[0].a.x = std::numeric_limits<double>::quiet_NaN();
        EXPECT_THROW(([&] { CollisionMesh bad{ invalid }; })(), std::exception)
          << "nonfinite vertex rejection";
        EXPECT_THROW(([&] { CollisionMesh bad{ std::span<const std::byte>{} }; })(), std::exception)
          << "truncated STL rejection";
        EXPECT_THROW(([&] { cube.intersects(cube, glm::dmat4{ 0 }); })(), std::exception)
          << "singular transform rejection";
    }

}
using namespace simulation;

TEST(SimulationKinematics, CadRestPoseAndGearing)
{
    const Profile profile{};
    Kinematics mechanics{ profile };
    const auto zero{ mechanics.joints({ 0, 0, 0 }) };
    ASSERT_TRUE((zero == std::array<double, 3>{ 0, 0, 0 })) << "straight pose is all-zero joint coordinates";
    const auto tip{ Kinematics::tcp(profile, zero) };
    ASSERT_TRUE((std::abs(tip[0] - 563.4) < 1e-9 && tip[1] == 0 && tip[2] == 28.9))
      << "CAD rest TCP preserved";
    const auto moved{ mechanics.joints({ 450, -900, 360 }) };
    ASSERT_TRUE((std::abs(moved[0] - std::numbers::pi / 2) < 1e-12 &&
                 std::abs(moved[1] + std::numbers::pi) < 1e-12 && std::abs(moved[2] - 1.25) < 1e-12))
      << "shared conversion: 5:1 rotary, 1.25 mm/rev Z";
}

TEST(SimulationKinematics, SignedOffsetsAndUnwrappedTurns)
{
    const Profile profile{};
    Kinematics mechanics{ profile };
    Profile signed_profile{ profile };
    signed_profile.direction[0] = ui::motion::AxisDirection::OppositeToMotor;
    signed_profile.direction[2] = ui::motion::AxisDirection::OppositeToMotor;
    signed_profile.shaft_reference = { 90, 180, 360 };
    signed_profile.axis_reference = { .2, -.3, 4 };
    const auto signed_joints{ Kinematics{ signed_profile }.joints({ 540, 180, 720 }) };
    ASSERT_TRUE((std::abs(signed_joints[0] - (.2 - std::numbers::pi / 2)) < 1e-12 &&
                 signed_joints[1] == -.3 && std::abs(signed_joints[2] - 2.75) < 1e-12))
      << "signed conversion and independent reference offsets";
    const auto turns{ mechanics.joints({ 3600, -5400, -720 }) };
    ASSERT_TRUE((std::abs(turns[0] - 4 * std::numbers::pi) < 1e-12 &&
                 std::abs(turns[1] + 6 * std::numbers::pi) < 1e-12 && std::abs(turns[2] + 2.5) < 1e-12))
      << "unwrapped joint coordinates";
}

TEST(ContactSensorsTest, RestPoseReleasesAllSwitches)
{
    const Profile profile{};
    ContactSensors contacts{ profile };
    for (unsigned i{}; i < 3; ++i)
        ASSERT_TRUE((!contacts.update(i, 0))) << "straight rest pose releases all switches";
}

class ContactAxisTest : public testing::TestWithParam<unsigned>
{
};

TEST_P(ContactAxisTest, GeometryHysteresisCachingAndRepeatability)
{
    const Profile profile{};
    ContactSensors contacts{ profile };
    const unsigned axis{ GetParam() };
    {
        const auto coordinate{ [&](double v) { return axis < 2 ? v * std::numbers::pi / 180 : v; } };
        const auto boundary{ [&](double deflection) {
            double lo{ axis == 0 ? 110.0 : axis == 1 ? 50.0 : 25.0 };
            double hi{ axis == 0 ? 115.0 : axis == 1 ? 60.0 : 35.0 };
            EXPECT_TRUE((ContactSensors::clearance(profile, axis, coordinate(lo), 0) > .05 &&
                         ContactSensors::contact(profile, axis, coordinate(hi), 2)))
              << "geometry brackets first home contact";
            for (unsigned i{}; i < 30; ++i) {
                const double mid{ (lo + hi) / 2 };
                if (deflection < 0 ? ContactSensors::clearance(profile, axis, coordinate(mid), 0) <= .05
                                   : ContactSensors::contact(profile, axis, coordinate(mid), deflection))
                    hi = mid;
                else
                    lo = mid;
            }
            return (lo + hi) / 2;
        } };
        const double operate{ boundary(2) }, release{ boundary(-1) }, between{ (operate + release) / 2 };
        ASSERT_TRUE((operate > release + .0001)) << "lever deflection creates physical release hysteresis";
        ASSERT_TRUE((!contacts.update(axis, coordinate(between)))) << "not yet operated on approach";
        ASSERT_TRUE((contacts.update(axis, coordinate(operate + .001)))) << "operated by collision geometry";
        ASSERT_TRUE((contacts.update(axis, coordinate(between))))
          << "contact holds through backoff hysteresis";
        ASSERT_TRUE((!contacts.update(axis, coordinate(release - .001))))
          << "contact releases below release deflection";
        ASSERT_TRUE((contacts.update(axis, coordinate(operate + .001))))
          << "second latch repeats the contact";
        ContactSensors cached{ profile };
        bool oracle{};
        for (unsigned pass{}; pass < 4; ++pass) {
            for (unsigned step{}; step <= 60; ++step) {
                const double fraction{ (pass % 2) ? 1 - step / 60.0 : step / 60.0 };
                const double q{ coordinate(release - .02 + fraction * (operate - release + .04)) };
                oracle =
                  oracle
                    ? ContactSensors::clearance(profile, axis, q, 0) <=
                        profile.sensor_release_clearance_mm[axis]
                    : ContactSensors::contact(profile, axis, q, profile.sensor_operate_deflection_deg[axis]);
                ASSERT_TRUE((cached.update(axis, q) == oracle))
                  << "clearance cache agrees with exact queries across approach/backoff/reversal";
            }
        }
        if (axis < 2)
            ASSERT_TRUE((ContactSensors::contact(profile, axis, coordinate(operate + .01 + 360), 2)))
              << "rotary contacts repeat with physical orientation";
    }
}

INSTANTIATE_TEST_SUITE_P(AllAxes, ContactAxisTest, testing::Values(0U, 1U, 2U));

TEST(ContactSensorsTest, DisplacedGeometryRemovesContact)
{
    const Profile profile{};
    Profile displaced{ profile };
    displaced.geometry_mm[0] += 1000;
    ASSERT_TRUE((!ContactSensors::contact(displaced, 1, 60 * std::numbers::pi / 180, 2)))
      << "moving the joint away removes contact; no fixed angle shortcut";
}
