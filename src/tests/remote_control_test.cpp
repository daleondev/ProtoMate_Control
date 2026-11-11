#include "ui/motion/Client.hpp"
#include <gtest/gtest.h>
#include <thread>
using namespace std::chrono_literals;
namespace {
    using Client = ui::motion::Client;
    struct RemoteControl : testing::Test {
        pnm::msg::Bus bus;
        Client client{bus};
        void SetUp() override { client.setConnected(true); }
        static bool wait(std::stop_token token) {
            auto deadline = std::chrono::steady_clock::now() + 2s;
            while (!token.stop_requested() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
            return token.stop_requested();
        }
    };
}
TEST_F(RemoteControl, PermissionIsLocalAndExpiredGenerationsCannotRevive) {
    EXPECT_FALSE(client.remotePermission()); EXPECT_EQ(client.openRemote(), 0);
    client.remotePermission(true);
    const auto source = client.openRemote(); ASSERT_NE(source, 0);
    client.remotePermission(false);
    EXPECT_FALSE(client.remoteAuthorized(source)); EXPECT_FALSE(client.renewRemote(source, {}));
    EXPECT_THROW(client.create(motion::Family::Axis, 1, source), std::runtime_error);
    client.remotePermission(true);
    EXPECT_FALSE(client.remoteAuthorized(source));
    EXPECT_GT(client.openRemote(), source);
}
TEST_F(RemoteControl, RawQueuesCannotMixLocalOrDifferentRemoteSources) {
    client.remotePermission(true);
    const auto source = client.openRemote(), other = client.openRemote();
    ASSERT_NE(source, 0); ASSERT_NE(other, 0); EXPECT_EQ(client.openRemote(), 0);
    const auto local = client.create(motion::Family::Motor, 1);
    EXPECT_THROW(client.create(motion::Family::Motor, 1, source), std::runtime_error);
    EXPECT_NO_THROW(client.create(motion::Family::Motor, 1));
    const auto remote = client.create(motion::Family::Motor, 2, source);
    EXPECT_THROW(client.create(motion::Family::Motor, 2), std::runtime_error);
    EXPECT_THROW(client.create(motion::Family::Motor, 2, other), std::runtime_error);
    EXPECT_NO_THROW(client.create(motion::Family::Motor, 2, source));
    EXPECT_FALSE(client.cancellation(local).stop_requested());
    EXPECT_FALSE(client.cancellation(remote).stop_requested());
}
TEST_F(RemoteControl, IccLossRevokesPermissionAndReconnectNeverReplaysIt) {
    client.remotePermission(true);
    const auto source=client.openRemote();
    ASSERT_NE(source,0);
    client.setConnected(false);
    EXPECT_FALSE(client.remotePermission());
    client.remotePermission(true);
    EXPECT_FALSE(client.remotePermission());
    client.setConnected(true);
    EXPECT_FALSE(client.remotePermission());
    EXPECT_FALSE(client.remoteAuthorized(source));
    EXPECT_FALSE(client.renewRemote(source,{}));
}
TEST_F(RemoteControl, ExpiryCancelsOnlyRemotePreparationWithoutTheServerWorker) {
    client.remotePermission(true);
    const auto source = client.openRemote();
    const auto remote = client.create(motion::Family::Axis, 1, source);
    const auto local = client.create(motion::Family::Axis, 2);
    auto cancellation = client.cancellation(remote);
    std::jthread worker([&](std::stop_token stop) { client.run(stop); });
    ASSERT_TRUE(wait(cancellation));
    EXPECT_FALSE(client.cancellation(local).stop_requested());
    EXPECT_FALSE(client.renewRemote(source, {}));
}
TEST_F(RemoteControl, OmittedHeldJobStopsWhileOtherJobsRemainSupervised) {
    client.remotePermission(true);
    const auto source = client.openRemote();
    const auto first = client.create(motion::Family::Axis, 1, source, true);
    const auto second = client.create(motion::Family::Axis, 2, source, true);
    const std::array held{second};
    ASSERT_TRUE(client.renewRemote(source, held));
    auto cancellation = client.cancellation(first);
    std::jthread worker([&](std::stop_token stop) { client.run(stop); });
    ASSERT_TRUE(wait(cancellation));
    EXPECT_FALSE(client.cancellation(second).stop_requested());
    const std::array revive{first, second};
    EXPECT_FALSE(client.renewRemote(source, revive));
    client.closeRemote(source);
    EXPECT_TRUE(wait(client.cancellation(second)));
}
TEST_F(RemoteControl, GuiGestureDeadlineRemainsIndependent) {
    const auto local = client.create(motion::Family::Axis, 1);
    client.superviseJog(local);
    auto cancellation = client.cancellation(local);
    const auto start = std::chrono::steady_clock::now();
    std::jthread worker([&](std::stop_token stop) { client.run(stop); });
    ASSERT_TRUE(wait(cancellation));
    EXPECT_LT(std::chrono::steady_clock::now() - start, 500ms);
}
