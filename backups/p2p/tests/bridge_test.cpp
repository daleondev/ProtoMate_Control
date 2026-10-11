// Numeric literals are test inputs and independently specified expected results.
// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
#include "p2p/Bridge.hpp"
#include "p2p/InProcessTransport.hpp"
#include <gtest/gtest.h>
#include <thread>
using namespace std::chrono_literals;
namespace
{
    template<typename T>
    T& require_value(std::optional<T>& value)
    {
        if (!value)
            throw std::logic_error("expected deferred operation to be present");
        return *value;
    }
    struct Pair
    {
        Pair() = default;
        Pair(const Pair&) = delete;
        Pair& operator=(const Pair&) = delete;
        Pair(Pair&&) = delete;
        Pair& operator=(Pair&&) = delete;
        pnm::msg::Bus ui, io;
        pnm::coro::Context uic, ioc;
        std::array<std::shared_ptr<p2p::InProcessTransport>, 2> transport = p2p::InProcessTransport::pair();
        p2p::Bridge a{ ui, uic, transport[0] }, b{ io, ioc, transport[1] };
        void pump()
        {
            for (int i = 0; i < 20; ++i) {
                uic.poll();
                ioc.poll();
            }
        }
        void start()
        {
            a.start();
            b.start();
            pump();
            ASSERT_EQ(a.state(), p2p::LinkState::Connected);
            ASSERT_EQ(b.state(), p2p::LinkState::Connected);
        }
        ~Pair()
        {
            try {
                a.close();
                b.close();
                pump();
            } catch (const std::exception& error) {
                ADD_FAILURE() << "bridge cleanup failed: " << error.what();
            }
        }
    };
    TEST(P2p, PortableIntegersAndMalformedFrames)
    {
        auto bytes = p2p::encode<std::uint32_t>(0x12345678);
        ASSERT_EQ(bytes.size(), 4);
        EXPECT_EQ(bytes[0], std::byte{ 0x78 });
        EXPECT_EQ(bytes[3], std::byte{ 0x12 });
        EXPECT_EQ(p2p::decode<std::uint32_t>(bytes), 0x12345678);
        bytes.push_back(std::byte{ 0 });
        EXPECT_THROW(p2p::decode<std::uint32_t>(bytes), std::invalid_argument);
        EXPECT_THROW(p2p::util::unpack(bytes), std::invalid_argument);
    }
    TEST(P2p, IntegerWidthsAndBooleanValidation)
    {
        EXPECT_EQ(p2p::encode<std::uint8_t>(0xff), (p2p::Bytes{ std::byte{ 0xff } }));
        EXPECT_EQ(p2p::decode<std::int8_t>(p2p::encode<std::int8_t>(-1)), -1);
        EXPECT_EQ(p2p::encode<std::uint16_t>(0x1234), (p2p::Bytes{ std::byte{ 0x34 }, std::byte{ 0x12 } }));
        const auto largest = std::numeric_limits<std::uint64_t>::max();
        EXPECT_EQ(p2p::decode<std::uint64_t>(p2p::encode(largest)), largest);
        const auto smallest = std::numeric_limits<std::int64_t>::min();
        EXPECT_EQ(p2p::decode<std::int64_t>(p2p::encode(smallest)), smallest);
        EXPECT_TRUE(p2p::decode<bool>(p2p::encode(true)));
        EXPECT_FALSE(p2p::decode<bool>(p2p::encode(false)));
        EXPECT_THROW(p2p::decode<bool>(p2p::Bytes{ std::byte{ 2 } }), std::invalid_argument);
        EXPECT_THROW(p2p::decode<std::uint16_t>(p2p::Bytes{ std::byte{ 1 } }), std::invalid_argument);
    }
    TEST(P2p, TransportDestructionDetachesWakeup)
    {
        auto pair = p2p::InProcessTransport::pair();
        unsigned wakes{};
        pair[0]->setWakeup([&] { ++wakes; });
        const auto before = wakes;
        pair[0].reset();
        EXPECT_EQ(wakes, before);
        EXPECT_EQ(pair[1]->trySend(p2p::Bytes{ std::byte{ 1 } }, p2p::Lane::Normal), p2p::SendResult::Sent);
        EXPECT_EQ(wakes, before);
    }
    TEST(P2p, IndependentUrgentCapacityAndCapacityWakeup)
    {
        auto pair = p2p::InProcessTransport::pair(
          { .normal_frames = 1, .urgent_frames = 1, .frame_bytes = 128, .urgent_bytes = 128 });
        p2p::Bytes v(4);
        int wakes{};
        pair[0]->setWakeup([&] { ++wakes; });
        EXPECT_EQ(pair[0]->trySend(v, p2p::Lane::Normal), p2p::SendResult::Sent);
        EXPECT_EQ(pair[0]->trySend(v, p2p::Lane::Normal), p2p::SendResult::Full);
        v[0] = std::byte{ 1 };
        EXPECT_EQ(pair[0]->trySend(v, p2p::Lane::Urgent), p2p::SendResult::Sent);
        p2p::Bytes out;
        ASSERT_TRUE(pair[1]->tryReceive(out));
        EXPECT_EQ(out[0], std::byte{ 1 });
        EXPECT_GT(wakes, 1);
    }
    TEST(P2p, ServiceRoundTripAndLatestTopic)
    {
        Pair p;
        p.a.importService<int, int>(1, "double");
        p.b.exportService<int, int>(1, "double");
        p.b.publishTopic<int>(2, "state");
        p.a.receiveTopic<int>(2, "state");
        auto service = p.io.service<int, int>("double").serve([](int v) { return 2 * v; });
        int seen{};
        auto sub = p.ui.topic<int>("state").subscribe([&](int v) { seen = v; },
                                                      pnm::msg::SubscriptionOptions::latestOnly());
        p.start();
        auto call = p.ui.service<int, int>("double").request(21, 1s);
        p.pump();
        service.poll();
        p.pump();
        ASSERT_TRUE(call.ready());
        EXPECT_EQ(call.get().value(), 42);
        for (int i = 0; i < 100; ++i)
            p.io.topic<int>("state").publish(i);
        p.pump();
        sub.poll();
        EXPECT_EQ(seen, 99);
    }
    TEST(P2p, ActionAdmissionCancellationAndCleanup)
    {
        Pair p;
        p.a.importAction<int, int, int>(3, "move");
        p.b.exportAction<int, int, int>(3, "move");
        std::optional<pnm::msg::ActionExecution<int, int>> execution;
        auto provider = p.io.action<int, int, int>("move").serveDeferred(
          [&](int, pnm::msg::ActionExecution<int, int> e) { execution.emplace(std::move(e)); });
        bool accepted{};
        bool done{};
        int feedback{};
        p.start();
        auto goal = p.ui.action<int, int, int>("move").sendGoal(5, { 1s }, { .on_accepted = [&] {
            accepted = true;
        }, .on_feedback = [&](int f) { feedback = f; }, .on_result = [&](auto r) {
            done = true;
            ASSERT_TRUE(r);
            EXPECT_EQ(r->status, pnm::msg::ActionStatus::Cancelled);
            EXPECT_EQ(r->value, 17);
        } });
        p.pump();
        provider.poll();
        p.pump();
        goal.poll();
        EXPECT_FALSE(accepted);
        ASSERT_TRUE(execution);
        require_value(execution).accept();
        require_value(execution).feedback(7);
        p.pump();
        goal.poll();
        EXPECT_TRUE(accepted);
        EXPECT_EQ(feedback, 7);
        goal.requestCancel();
        p.pump();
        EXPECT_TRUE(require_value(execution).cancelRequested());
        EXPECT_FALSE(done);
        require_value(execution).cancelled(17);
        p.pump();
        goal.poll();
        EXPECT_TRUE(done);
    }
    TEST(P2p, CancellationCanOvertakeQueuedGoal)
    {
        Pair p;
        p.a.importAction<int, int, int>(1, "move");
        p.b.exportAction<int, int, int>(1, "move");
        int executed{};
        auto provider = p.io.action<int, int, int>("move").serve([&](int) {
            ++executed;
            return [](auto& e) { e.succeed(1); };
        });
        p.start();
        auto goal = p.ui.action<int, int, int>("move").sendGoal(1, { 1s }, { .on_result = [](auto) {} });
        p.uic.poll();
        goal.requestCancel();
        p.uic.poll();
        p.pump();
        provider.poll();
        p.pump();
        goal.poll();
        EXPECT_EQ(executed, 0);
    }
    TEST(P2p, DisconnectFailsOutstandingWithoutReplay)
    {
        Pair p;
        p.a.importService<int, int>(1, "request");
        p.b.exportService<int, int>(1, "request");
        int called{};
        auto provider = p.io.service<int, int>("request").serve([&](int v) {
            ++called;
            return v;
        });
        p.start();
        auto call = p.ui.service<int, int>("request").request(1, 1s);
        p.uic.poll();
        p.transport[0]->setConnected(false);
        p.pump();
        ASSERT_TRUE(call.ready());
        EXPECT_FALSE(call.get());
        p.transport[0]->setConnected(true);
        p.pump();
        provider.poll();
        p.pump();
        EXPECT_EQ(called, 0);
    }
    TEST(P2p, RejectionCarriesReasonWithoutAcceptance)
    {
        Pair p;
        p.a.importAction<int, int, int>(1, "move");
        p.b.exportAction<int, int, int>(1, "move");
        auto provider = p.io.action<int, int, int>("move").serveDeferred(
          [](int, auto execution) { EXPECT_TRUE(execution.reject(42)); });
        bool accepted{};
        bool done{};
        p.start();
        auto goal = p.ui.action<int, int, int>("move").sendGoal(
          1, { 1s }, { .on_accepted = [&] { accepted = true; }, .on_result = [&](auto result) {
            ASSERT_TRUE(result);
            EXPECT_EQ(result->status, pnm::msg::ActionStatus::Rejected);
            EXPECT_EQ(result->value, 42);
            done = true;
        } });
        p.pump();
        provider.poll();
        p.pump();
        goal.poll();
        EXPECT_TRUE(done);
        EXPECT_FALSE(accepted);
    }
    TEST(P2p, RestartRenegotiatesWithoutReplayingOldSession)
    {
        Pair p;
        p.a.importService<int, int>(1, "request");
        p.b.exportService<int, int>(1, "request");
        int called{};
        auto provider = p.io.service<int, int>("request").serve([&](int value) {
            ++called;
            return value;
        });
        p.start();
        auto first = p.ui.service<int, int>("request").request(1, 1s);
        p.pump();
        p.b.close();
        p.pump();
        p2p::Bridge replacement{ p.io, p.ioc, p.transport[1] };
        replacement.exportService<int, int>(1, "request");
        replacement.start();
        p.pump();
        EXPECT_EQ(p.a.state(), p2p::LinkState::Connected);
        EXPECT_EQ(replacement.state(), p2p::LinkState::Connected);
        EXPECT_TRUE(first.ready());
        EXPECT_FALSE(first.get());
        provider.poll();
        p.pump();
        EXPECT_EQ(called, 0);
        auto second = p.ui.service<int, int>("request").request(2, 1s);
        p.pump();
        provider.poll();
        p.pump();
        ASSERT_TRUE(second.ready());
        EXPECT_EQ(second.get().value(), 2);
        EXPECT_EQ(called, 1);
        replacement.close();
        p.pump();
    }
    TEST(P2p, ServiceDeadlineCancelsDeferredProviderAndReleasesCapacity)
    {
        Pair p;
        p.a.importService<int, int>(1, "defer");
        p.b.exportService<int, int>(1, "defer");
        std::optional<pnm::msg::Reply<int>> reply;
        auto provider = p.io.service<int, int>("defer").serveDeferred(
          [&](int, auto value) { reply.emplace(std::move(value)); });
        p.start();
        for (int i = 0; i < 12; ++i) {
            auto call = p.ui.service<int, int>("defer").request(1, 2ms);
            p.pump();
            provider.poll();
            std::this_thread::sleep_for(3ms);
            p.pump();
            ASSERT_TRUE(call.ready());
            EXPECT_FALSE(call.get());
            ASSERT_TRUE(reply);
            EXPECT_FALSE(require_value(reply).pending());
        }
        auto call = p.ui.service<int, int>("defer").request(1, 1s);
        p.pump();
        provider.poll();
        require_value(reply).respond(9);
        p.pump();
        ASSERT_TRUE(call.ready());
        EXPECT_EQ(call.get().value(), 9);
    }
    TEST(P2p, SaturationIsBusyAndDoesNotRunExtraGoals)
    {
        Pair p;
        p.a.importAction<int, int, int>(1, "move");
        p.b.exportAction<int, int, int>(1, "move");
        std::vector<pnm::msg::ActionExecution<int, int>> executions;
        auto provider = p.io.action<int, int, int>("move").serveDeferred([&](int, auto e) {
            e.accept();
            executions.push_back(std::move(e));
        }, { 8 });
        p.start();
        std::vector<pnm::msg::PendingGoal<int, int>> goals;
        unsigned busy{};
        for (unsigned i = 0; i < 12; ++i) {
            goals.push_back(
              p.ui.action<int, int, int>("move").sendGoal(1, { 1s }, { .on_result = [&](auto r) {
                if (!r && r.error() == pnm::msg::ActionError::Busy)
                    ++busy;
            } }));
            p.pump();
            provider.poll();
            p.pump();
        }
        for (auto& g : goals)
            g.poll();
        EXPECT_EQ(executions.size(), 8);
        EXPECT_EQ(busy, 4);
        for (auto& e : executions)
            e.succeed(1);
        p.pump();
        for (auto& g : goals)
            g.poll();
        goals.push_back(p.ui.action<int, int, int>("move").sendGoal(1, { 1s }, { .on_result = [](auto) {} }));
        p.pump();
        provider.poll();
        EXPECT_EQ(executions.size(), 9);
    }
    TEST(P2p, HeartbeatExpiryCancelsAcceptedRemoteWork)
    {
        Pair p;
        p.a.importAction<int, int, int>(1, "move");
        p.b.exportAction<int, int, int>(1, "move");
        std::optional<pnm::msg::ActionExecution<int, int>> execution;
        auto provider = p.io.action<int, int, int>("move").serveDeferred([&](int, auto e) {
            e.accept();
            execution.emplace(std::move(e));
        });
        p.start();
        auto goal = p.ui.action<int, int, int>("move").sendGoal(1, { 1s }, { .on_result = [](auto) {} });
        p.pump();
        provider.poll();
        p.pump();
        std::this_thread::sleep_for(520ms);
        p.ioc.poll();
        EXPECT_EQ(p.b.state(), p2p::LinkState::TimedOut);
        ASSERT_TRUE(execution);
        EXPECT_TRUE(require_value(execution).cancelRequested());
    }

    class AckGate final : public p2p::ITransport
    {
      public:
        explicit AckGate(std::shared_ptr<p2p::InProcessTransport> endpoint)
          : inner(std::move(endpoint))
        {
        }
        std::shared_ptr<p2p::InProcessTransport> inner;
        bool block{};
        std::function<void()> notification;
        p2p::SendResult trySend(std::span<const std::byte> bytes, p2p::Lane lane) override
        {
            if (block && p2p::util::unpack(bytes).kind == p2p::util::Kind::Ack)
                return p2p::SendResult::Full;
            return inner->trySend(bytes, lane);
        }
        bool tryReceive(p2p::Bytes& bytes) override { return inner->tryReceive(bytes); }
        bool connected() const override { return inner->connected(); }
        p2p::Capacity capacity(p2p::Lane lane) const override { return inner->capacity(lane); }
        void setWakeup(std::function<void()> callback) override
        {
            notification = callback;
            inner->setWakeup(std::move(callback));
        }
        void release()
        {
            block = false;
            if (notification)
                notification();
        }
    };
    TEST(P2p, TerminalResultOccupiesCapacityUntilAcknowledgement)
    {
        pnm::msg::Bus ui;
        pnm::msg::Bus io;
        pnm::coro::Context uc;
        pnm::coro::Context ic;
        auto transport = p2p::InProcessTransport::pair();
        auto gate = std::make_shared<AckGate>(transport[0]);
        p2p::Bridge a(ui, uc, gate);
        p2p::Bridge b(io, ic, transport[1], { .max_pending = 1 });
        a.importAction<int, int, int>(1, "move");
        b.exportAction<int, int, int>(1, "move");
        int executed{};
        auto server = io.action<int, int, int>("move").serve([&](int) {
            ++executed;
            return [](auto& e) { e.succeed(9); };
        });
        auto pump = [&] {
            for (int i = 0; i < 20; ++i) {
                uc.poll();
                ic.poll();
            }
        };
        a.start();
        b.start();
        pump();
        gate->block = true;
        bool done{};
        bool busy{};
        auto first = ui.action<int, int, int>("move").sendGoal(1, { 1s }, { .on_result = [&](auto r) {
            ASSERT_TRUE(r);
            EXPECT_EQ(r->value, 9);
            done = true;
        } });
        pump();
        server.poll();
        pump();
        first.poll();
        EXPECT_TRUE(done);
        auto second = ui.action<int, int, int>("move").sendGoal(2, { 1s }, { .on_result = [&](auto r) {
            ASSERT_FALSE(r);
            busy = r.error() == pnm::msg::ActionError::Busy;
        } });
        pump();
        server.poll();
        pump();
        second.poll();
        EXPECT_TRUE(busy);
        EXPECT_EQ(executed, 1);
        gate->release();
        pump();
        auto third = ui.action<int, int, int>("move").sendGoal(
          3, { 1s }, { .on_result = [](auto r) { EXPECT_TRUE(r); } });
        pump();
        server.poll();
        pump();
        third.poll();
        EXPECT_EQ(executed, 2);
        a.close();
        b.close();
        pump();
    }
    TEST(P2p, ObsoleteFramesAndMalformedVersionsCannotReachSubscribers)
    {
        pnm::msg::Bus ui;
        pnm::msg::Bus io;
        pnm::coro::Context uc;
        pnm::coro::Context ic;
        auto transport = p2p::InProcessTransport::pair();
        p2p::Bridge a(ui, uc, transport[0], { .session = 101 });
        p2p::Bridge b(io, ic, transport[1], { .session = 202 });
        a.receiveTopic<int>(1, "state");
        b.publishTopic<int>(1, "state");
        int seen{};
        auto subscription = ui.topic<int>("state").subscribe([&](int v) { seen = v; });
        auto pump = [&] {
            for (int i = 0; i < 20; ++i) {
                uc.poll();
                ic.poll();
            }
        };
        a.start();
        b.start();
        pump();
        transport[0]->setConnected(false);
        pump();
        transport[0]->setConnected(true);
        pump();
        const auto obsolete = p2p::util::pack({ .kind = p2p::util::Kind::Topic,
                                                .endpoint = 1,
                                                .source = 202,
                                                .destination = 101,
                                                .payload = p2p::encode(99) });
        transport[1]->trySend(obsolete, p2p::Lane::Normal);
        pump();
        subscription.poll();
        EXPECT_EQ(seen, 0);
        io.topic<int>("state").publish(5);
        pump();
        subscription.poll();
        EXPECT_EQ(seen, 5);
        auto invalid = obsolete;
        invalid[4] = std::byte{ 255 };
        transport[1]->trySend(invalid, p2p::Lane::Normal);
        uc.poll();
        EXPECT_EQ(a.state(), p2p::LinkState::ProtocolError);
        subscription.poll();
        EXPECT_EQ(seen, 5);
        a.close();
        b.close();
        pump();
    }

}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
