#include "runtime/synchronization/Notification.hpp"
#include <future>
#include <gtest/gtest.h>
#include <thread>

using namespace std::chrono_literals;

TEST(RuntimeNotification, RetainsEarlySignalsCoalescesAndDoesNotReturnEarlyOnTimeout)
{
    runtime::Notification event;
    event.signal();
    event.signal();
    EXPECT_TRUE(event.waitUntil(std::chrono::steady_clock::now()));
    EXPECT_FALSE(event.waitUntil(std::chrono::steady_clock::now()));
    event.signal();
    event.clear();
    const auto deadline{ std::chrono::steady_clock::now() + 5ms };
    EXPECT_FALSE(event.waitUntil(deadline));
    EXPECT_GE(std::chrono::steady_clock::now(), deadline);
}

TEST(RuntimeNotification, WakesAnIndefinitelyBlockedConsumerAndSupportsCancellation)
{
    runtime::Notification event;
    std::promise<bool> finished;
    auto future{ finished.get_future() };
    std::jthread worker{ [&](std::stop_token stop) {
        const std::stop_callback cancellation{ stop, [&] { event.signal(); } };
        finished.set_value(event.waitUntil(std::chrono::steady_clock::time_point::max()));
    } };
    EXPECT_EQ(future.wait_for(5ms), std::future_status::timeout);
    worker.request_stop();
    ASSERT_EQ(future.wait_for(100ms), std::future_status::ready);
    EXPECT_TRUE(future.get());
}
