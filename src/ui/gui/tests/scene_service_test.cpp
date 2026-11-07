#include "runtime/thread.hpp"
#include "ui/gui/scene3d/DetailPolicy.hpp"
#include "ui/gui/scene3d/RenderService.hpp"
#include <algorithm>
#include <chrono>
#include <gtest/gtest.h>
#include <thread>
using namespace ui::gui::scene3d;
using namespace std::chrono_literals;
namespace
{
    template<class F>
    bool eventually(F predicate)
    {
        const auto end = std::chrono::steady_clock::now() + 2s;
        while (!predicate()) {
            if (std::chrono::steady_clock::now() >= end) {
                return false;
            }
            std::this_thread::sleep_for(1ms);
        }
        return true;
    }
    Frame request(unsigned n) { return { &benchmarkScene(2000), benchmarkSnapshot(float(n) * .1f, n, 0) }; }
}
TEST(SceneService, LatestPendingAndImmutableFrontUntilReleased)
{
    RenderService service;
    EXPECT_FALSE(service.submit(request(1)));
    service.visible(true);
    for (unsigned i = 1; i <= 100; ++i) {
        ASSERT_TRUE(service.submit(request(i)));
    }
    EXPECT_EQ(service.status().replaced, 99U);
    auto worker = runtime::thread::create_jthread({ .priority = 16, .stack_size = 16384 },
                                                  [&](std::stop_token stop) { service.run(stop); });
    ASSERT_TRUE(eventually([&] { return service.status().rendered == 1; }));
    const auto front = service.acquire();
    ASSERT_TRUE(front);
    EXPECT_EQ(front->revision, 100U);
    const std::vector saved(front->pixels.begin(), front->pixels.end());
    ASSERT_TRUE(service.submit(request(101)));
    ASSERT_TRUE(eventually([&] { return service.status().rendered == 2; }));
    ASSERT_TRUE(service.submit(request(102)));
    ASSERT_TRUE(service.submit(request(103)));
    std::this_thread::sleep_for(70ms);
    EXPECT_EQ(service.status().rendered, 2U); // ready + displayed own both slots
    EXPECT_TRUE(std::ranges::equal(saved, front->pixels));
    const auto next = service.acquire();
    ASSERT_TRUE(next);
    EXPECT_EQ(next->revision, 101U);
    ASSERT_TRUE(eventually([&] { return service.status().rendered == 3; }));
    EXPECT_EQ(service.acquire()->revision, 103U);
    service.visible(false);
    EXPECT_FALSE(service.submit(request(104)));
    service.stopAndWait();
    worker.join();
    EXPECT_FALSE(service.status().running);
    EXPECT_FALSE(service.status().failed);
}
TEST(SceneService, StopDuringRenderingAndRestartVisibility)
{
    RenderService service;
    service.visible(true);
    auto worker = runtime::thread::create_jthread({ .priority = 16, .stack_size = 16384 },
                                                  [&](std::stop_token stop) { service.run(stop); });
    ASSERT_TRUE(service.submit(request(1)));
    ASSERT_TRUE(eventually([&] { return service.status().rendered > 0; }));
    service.visible(false);
    service.visible(true);
    EXPECT_FALSE(service.acquire());
    ASSERT_TRUE(service.submit(request(2)));
    worker.request_stop();
    worker.join();
    EXPECT_FALSE(service.status().running);
    EXPECT_FALSE(service.status().failed);
}

TEST(RenderPacing, SamePolicyBoundsCadenceAndLeavesRestAfterExpensiveFrames)
{
    const auto start = std::chrono::steady_clock::time_point{};
    EXPECT_EQ(nextRender(start, start + 20ms, LIVE_MAX_FPS), start + 200ms);
    EXPECT_EQ(nextRender(start, start + 400ms, LIVE_MAX_FPS), start + 500ms);
    EXPECT_EQ(nextRender(start, start + 200ms, 2), start + 500ms);
}

TEST(RobotDetail, RendersOnChangeAndRefinesOnceAfterSettling)
{
    DetailPolicy policy;
    const auto start = DetailPolicy::Clock::time_point{};
    ASSERT_EQ(policy.request(start), RobotDetail::Full);
    policy.submitted(RobotDetail::Full);
    EXPECT_FALSE(policy.request(start + 10s));
    policy.changed(start + 11s);
    ASSERT_EQ(policy.request(start + 11s), RobotDetail::Interactive);
    policy.submitted(RobotDetail::Interactive);
    EXPECT_FALSE(policy.request(start + 11499ms));
    EXPECT_EQ(policy.request(start + 11500ms), RobotDetail::Full);
    // A rejected submission remains pending, including refinement.
    EXPECT_EQ(policy.request(start + 12s), RobotDetail::Full);
    policy.submitted(RobotDetail::Full);
    EXPECT_FALSE(policy.request(start + 20s));
    policy.invalidate(); // Reopening a hidden view recreates its released image.
    EXPECT_EQ(policy.request(start + 20s), RobotDetail::Full);
}

TEST(RobotDetail, ContinuingChangesPostponeRefinementAndRestartAfterSettling)
{
    DetailPolicy policy;
    const auto start = DetailPolicy::Clock::time_point{};
    for (unsigned i = 0; i < 50; ++i) {
        const auto now = start + i * 50ms;
        policy.changed(now);
        EXPECT_EQ(policy.request(now), RobotDetail::Interactive);
        policy.submitted(RobotDetail::Interactive);
        EXPECT_FALSE(policy.request(now + 49ms));
    }
    EXPECT_EQ(policy.request(start + 3s), RobotDetail::Full);
    policy.submitted(RobotDetail::Full);
    policy.changed(start + 4s);
    EXPECT_EQ(policy.request(start + 4s), RobotDetail::Interactive);
}

TEST(SceneService, LatestPoseReplacesPendingWorkDuringPacingAndCancellationWakesImmediately)
{
    RenderService service;
    service.visible(true);
    auto worker = runtime::thread::create_jthread({ .priority = 16, .stack_size = 16384 },
                                                  [&](std::stop_token stop) { service.run(stop); });
    auto first = request(1);
    first.maximumFps = 1;
    ASSERT_TRUE(service.submit(first));
    ASSERT_TRUE(eventually([&] { return service.status().rendered == 1; }));
    ASSERT_TRUE(service.acquire());
    for (unsigned i = 2; i <= 8; ++i) {
        auto next = request(i);
        next.maximumFps = 1;
        ASSERT_TRUE(service.submit(next));
    }
    ASSERT_TRUE(eventually([&] { return service.status().rendered == 2; }));
    ASSERT_EQ(service.acquire()->revision, 8U);
    const auto start = std::chrono::steady_clock::now();
    worker.request_stop();
    worker.join();
    EXPECT_LT(std::chrono::steady_clock::now() - start, 100ms);
}

TEST(SceneService, RejectsInvalidPacing)
{
    RenderService service;
    service.visible(true);
    auto frame = request(1);
    frame.maximumFps = 0;
    EXPECT_FALSE(service.submit(frame));
    frame.maximumFps = 31;
    EXPECT_FALSE(service.submit(frame));
}
