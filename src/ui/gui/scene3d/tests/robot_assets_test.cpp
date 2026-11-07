#include "robot_base_link_asset.hpp"
#include "robot_link_1_asset.hpp"
#include "robot_link_2_asset.hpp"
#include "robot_link_3_asset.hpp"
#include "ui/gui/scene3d/Renderer.hpp"
#include "ui/gui/scene3d/Robot.hpp"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <vector>

using namespace ui::gui::scene3d;

namespace
{
    Scene fixtureScene()
    {
        Scene scene{};
        scene.count = 4;
        scene.nodes[0] = { &assets::robot_base_link, -1 };
        scene.nodes[1] = { &assets::robot_link_1, -1 };
        scene.nodes[2] = { &assets::robot_link_2, 1 };
        scene.nodes[3] = { &assets::robot_link_3, 2 };
        for (unsigned i = 0; i < scene.count; ++i) {
            scene.nodes[i].material.useMeshMaterial = true;
        }
        return scene;
    }

    SceneSnapshot pose(float shoulder, float elbow, float z)
    {
        SceneSnapshot snapshot{};
        // CAD rest registration, metres/Z-up; no recentering or scale fitting.
        snapshot.transforms[1].translation = { 0, 0, .052f };
        snapshot.transforms[1].rotation = rotation({ 0, 0, 1 }, shoulder);
        snapshot.transforms[2].translation = { .205f, 0, .3179f };
        snapshot.transforms[2].rotation = rotation({ 0, 0, 1 }, elbow);
        snapshot.transforms[3].translation = { .3584f, 0, -.061f + z };
        snapshot.camera.target = { .2f, 0, .21f };
        snapshot.camera.distance = .95f;
        snapshot.camera.nearPlane = .01f;
        snapshot.camera.farPlane = 4;
        return snapshot;
    }

    void exportFrame(const std::vector<std::uint16_t>& pixels, unsigned index, const char* name = "robot")
    {
        const auto* directory = std::getenv("SCENE3D_CAPTURE_DIR");
        if (!directory) {
            return;
        }
        std::filesystem::create_directories(directory);
        std::ofstream file(std::filesystem::path(directory) /
                             (std::string(name) + "-" + std::to_string(index) + ".ppm"),
                           std::ios::binary);
        file << "P6\n" << WIDTH << ' ' << HEIGHT << "\n255\n";
        for (std::size_t i = 1; i <= PIXELS; ++i) {
            const auto p = pixels[i];
            const char rgb[]{ static_cast<char>(((p >> 11) & 31) * 255 / 31),
                              static_cast<char>(((p >> 5) & 63) * 255 / 63),
                              static_cast<char>((p & 31) * 255 / 31) };
            file.write(rgb, 3);
        }
        ASSERT_TRUE(file.good());
    }
}

TEST(RobotVisuals, LocalFramesAndTextureFreeMaterials)
{
    const auto scene = fixtureScene();
    ASSERT_TRUE(scene.valid());
    EXPECT_NEAR(assets::robot_link_3.bounds.minimum.z, -.280f, .000001f);
    EXPECT_NEAR(assets::robot_base_link.bounds.minimum.z, 0, .000001f);
    for (unsigned i = 0; i < scene.count; ++i) {
        const auto& mesh = *static_cast<const tgx::Mesh3Dv2<tgx::RGB565>*>(scene.nodes[i].mesh->data);
        ASSERT_GT(mesh.nb_materials, 1);
        ASSERT_GT(mesh.nb_meshlets, 0);
        for (unsigned m = 0; m < mesh.nb_materials; ++m) {
            EXPECT_EQ(mesh.materials[m].texture, nullptr);
        }
    }
}

TEST(RobotVisuals, ArticulatedRendersAreDeterministicAndPreserveBufferGuards)
{
    const auto scene = fixtureScene();
    const SceneSnapshot poses[]{ pose(0, 0, 0), pose(.7f, -1.75f, .04f), pose(-1.2f, 2.1f, -.02f) };
    std::vector<std::uint16_t> color(PIXELS + 2, 0xa55a), repeat(color), depth(color);
    for (unsigned i = 0; i < std::size(poses); ++i) {
        ASSERT_TRUE(render(
          { &scene, poses[i] }, std::span(color).subspan(1, PIXELS), std::span(depth).subspan(1, PIXELS)));
        ASSERT_TRUE(render(
          { &scene, poses[i] }, std::span(repeat).subspan(1, PIXELS), std::span(depth).subspan(1, PIXELS)));
        EXPECT_EQ(color, repeat);
        EXPECT_EQ(color.front(), 0xa55a);
        EXPECT_EQ(color.back(), 0xa55a);
        EXPECT_EQ(depth.front(), 0xa55a);
        EXPECT_EQ(depth.back(), 0xa55a);
        EXPECT_GT(std::count_if(color.begin() + 1, color.end() - 1, [](auto p) { return p != 0x10c4; }),
                  8000);
        exportFrame(color, i);
    }
    auto monochrome = scene;
    for (unsigned i = 0; i < monochrome.count; ++i) {
        monochrome.nodes[i].material.useMeshMaterial = false;
    }
    ASSERT_TRUE(render(
      { &monochrome, poses[2] }, std::span(repeat).subspan(1, PIXELS), std::span(depth).subspan(1, PIXELS)));
    EXPECT_NE(color, repeat);
}

TEST(RobotVisuals, InteractiveDetailPreservesRegistrationAndReviewedSilhouette)
{
    auto full = robotScene(RobotDetail::Full);
    auto low = robotScene(RobotDetail::Interactive);
    ASSERT_TRUE(full.valid());
    ASSERT_TRUE(low.valid());
    ASSERT_EQ(full.count, low.count);
    unsigned total{};
    for (unsigned i = 0; i < full.count; ++i) {
        EXPECT_EQ(full.nodes[i].parent, low.nodes[i].parent);
        EXPECT_TRUE(low.nodes[i].material.useMeshMaterial);
        EXPECT_LT(low.nodes[i].mesh->triangles, full.nodes[i].mesh->triangles);
        total += low.nodes[i].mesh->triangles;
    }
    EXPECT_LT(total, 9000U);
    // Exclude the ground plane from the silhouette comparison.
    full.groundGrid = low.groundGrid = false;
    std::vector<std::uint16_t> a(PIXELS + 2, 0xa55a), b(a), da(a), db(a);
    const std::array<double, 3> joints[]{ { 0, 0, 0 }, { .7, -1.75, .04 }, { -1.2, 2.1, -.02 } };
    unsigned view{};
    double worst = 1;
    for (auto q : joints) {
        for (float pitch : { .45f, -.45f, 1.25f }) {
            for (float yaw : { 0.f, 1.5707963f, 3.1415926f, 4.712389f }) {
                auto snapshot = robotSnapshot(q);
                snapshot.camera.pitch = pitch;
                snapshot.camera.yaw = yaw;
                ASSERT_TRUE(render(
                  { &full, snapshot }, std::span(a).subspan(1, PIXELS), std::span(da).subspan(1, PIXELS)));
                ASSERT_TRUE(render(
                  { &low, snapshot }, std::span(b).subspan(1, PIXELS), std::span(db).subspan(1, PIXELS)));
                unsigned intersection{}, unionCount{};
                for (unsigned p = 1; p <= PIXELS; ++p) {
                    const bool left = da[p] != 0, right = db[p] != 0;
                    intersection += left && right;
                    unionCount += left || right;
                }
                ASSERT_GT(unionCount, 1000U);
                const auto iou = double(intersection) / unionCount;
                worst = std::min(worst, iou);
                std::printf("LOD view=%u silhouette_iou=%.6f\n", view, iou);
                EXPECT_GT(iou, .98) << "view " << view;
                for (const auto* v : { &a, &b, &da, &db }) {
                    EXPECT_EQ(v->front(), 0xa55a);
                    EXPECT_EQ(v->back(), 0xa55a);
                }
                exportFrame(a, view, "full");
                exportFrame(b, view++, "interactive");
            }
        }
    }
    std::printf("LOD triangles=%u minimum_silhouette_iou=%.6f\n", total, worst);
}
