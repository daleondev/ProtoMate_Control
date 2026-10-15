#include "ui/gui/scene3d/Renderer.hpp"
#include <algorithm>
#include <array>
#include <gtest/gtest.h>
#include <limits>
#include <set>
#include <vector>
using namespace ui::gui::scene3d;
namespace
{
    struct Buffers
    {
        std::vector<std::uint16_t> color = std::vector<std::uint16_t>(PIXELS + 2, 0xa55a);
        std::vector<std::uint16_t> depth = std::vector<std::uint16_t>(PIXELS + 2, 0xa55a);
        bool draw(const Scene& scene, const SceneSnapshot& snapshot)
        {
            return render(
              { &scene, snapshot }, std::span(color).subspan(1, PIXELS), std::span(depth).subspan(1, PIXELS));
        }
        void guards() const
        {
            EXPECT_EQ(color.front(), 0xa55a);
            EXPECT_EQ(color.back(), 0xa55a);
            EXPECT_EQ(depth.front(), 0xa55a);
            EXPECT_EQ(depth.back(), 0xa55a);
        }
    };
}
TEST(Scene3D, ShadedDeterministicAndBounded)
{
    Buffers a, b;
    auto pose = benchmarkSnapshot(1, 1, 0);
    ASSERT_TRUE(a.draw(benchmarkScene(2000), pose));
    ASSERT_TRUE(b.draw(benchmarkScene(2000), pose));
    EXPECT_EQ(a.color, b.color);
    EXPECT_GT(std::count_if(a.color.begin(), a.color.end(), [](auto c) { return c != 0x10c4; }), 60000);
    EXPECT_GT(std::set(a.color.begin(), a.color.end()).size(), 128U);
    a.guards();
    b.guards();
}
TEST(Scene3D, HierarchyAndDepthDoNotDependOnRootDrawOrder)
{
    Buffers a, b;
    Scene scene = benchmarkScene(2000);
    auto pose = benchmarkSnapshot(.7f, 1, 0);
    ASSERT_TRUE(a.draw(scene, pose));
    std::reverse(scene.nodes.begin(), scene.nodes.begin() + scene.count);
    std::reverse(pose.transforms.begin(), pose.transforms.begin() + scene.count);
    ASSERT_TRUE(b.draw(scene, pose));
    unsigned differences{};
    for (unsigned i = 0; i < a.color.size(); ++i) {
        differences += a.color[i] != b.color[i];
    }
    // Intersecting surfaces can tie after 16-bit depth quantization.
    EXPECT_LT(differences, 64U);
    scene = benchmarkScene(2000);
    pose = benchmarkSnapshot(.7f, 1, 0);
    // Parent at origin: the same local pose must produce identical pixels.
    scene.count = 2;
    scene.nodes[1] = scene.nodes[0];
    scene.nodes[1].parent = 0;
    pose.transforms[0] = {};
    pose.transforms[1].translation = { 2, 0, 0 };
    ASSERT_TRUE(a.draw(scene, pose));
    scene.nodes[1].parent = -1;
    ASSERT_TRUE(b.draw(scene, pose));
    EXPECT_EQ(a.color, b.color);
}
TEST(Scene3D, NearPlaneAndViewportClippingPreserveGuards)
{
    Buffers b;
    auto pose = benchmarkSnapshot(.3f, 1, 0);
    pose.camera.distance = .8f;
    pose.camera.nearPlane = .5f;
    ASSERT_TRUE(b.draw(benchmarkScene(5000), pose));
    b.guards();
}
TEST(Scene3D, RejectsInvalidHierarchyAndTransformsBeforeDrawing)
{
    Buffers b;
    Scene scene = benchmarkScene(2000);
    auto pose = benchmarkSnapshot(0, 1, 0);
    scene.nodes[0].parent = 1;
    EXPECT_FALSE(b.draw(scene, pose));
    scene = benchmarkScene(2000);
    pose.transforms[0].scale = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(b.draw(scene, pose));
    EXPECT_TRUE(std::ranges::all_of(b.color, [](auto c) { return c == 0xa55a; }));
    EXPECT_FALSE(render({ &scene, benchmarkSnapshot(0, 1, 0) }, std::span(b.color).first(3), b.depth));
}
