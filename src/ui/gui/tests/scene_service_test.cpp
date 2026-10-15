#include "runtime/thread.hpp"
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
