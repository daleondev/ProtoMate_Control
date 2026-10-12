#include "common/icc/bridge/Bridge.hpp"
#include "common/icc/transport/InProcessTransport.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <vector>

namespace
{
    using namespace std::chrono_literals;
    using Error = pnm::msg::ActionError;
    using Status = pnm::msg::ActionStatus;
    using Execution = pnm::msg::ActionExecution<std::uint32_t, std::uint32_t>;
    enum class Provider
    {
        Local,
        Remote
    };
    struct Actions
    {
        [[= Provider::Local]] pnm::msg::Action<std::uint32_t, std::uint32_t, std::uint32_t> local;
        [[= Provider::Remote]] pnm::msg::Action<std::uint32_t, std::uint32_t, std::uint32_t> remote;
    };
    // Deliberately independent from the bridge's encoder and enum representation.
    std::vector<std::byte> frame(unsigned kind,
                                 std::uint64_t id,
                                 std::string_view name = {},
                                 std::vector<std::byte> payload = {},
                                 unsigned budget = 0)
    {
        std::vector<std::byte> result(16 + name.size() + payload.size());
        result[0] = std::byte{ 7 };
        result[1] = static_cast<std::byte>(kind);
        result[2] = static_cast<std::byte>(name.size());
        result[3] = static_cast<std::byte>(name.size() >> 8);
        for (unsigned i = 0; i < 8; ++i)
            result[4 + i] = static_cast<std::byte>(id >> (8 * i));
        for (unsigned i = 0; i < 4; ++i)
            result[12 + i] = static_cast<std::byte>(budget >> (8 * i));
        for (std::size_t i = 0; i < name.size(); ++i)
            result[16 + i] = static_cast<std::byte>(name[i]);
        std::ranges::copy(payload, result.begin() + 16 + name.size());
        return result;
    }
    std::vector<std::byte> value(std::uint32_t number)
    {
        std::vector<std::byte> result(4);
        for (unsigned i = 0; i < 4; ++i)
            result[i] = static_cast<std::byte>(number >> (8 * i));
        return result;
    }
    std::vector<std::byte> terminal(unsigned status, std::uint32_t number)
    {
        auto result = value(number);
        result.insert(result.begin(), static_cast<std::byte>(status));
        return result;
    }
    template<typename T>
    pnm::coro::Task<void> capture(pnm::coro::Task<T> operation, std::shared_ptr<std::optional<T>> result)
    {
        result->emplace(co_await operation);
    }

    class ActionWireTest : public ::testing::Test
    {
      protected:
        pnm::msg::Bus bus;
        pnm::coro::Context context;
        std::array<std::shared_ptr<icc::InProcessTransport>, 2> endpoints{ icc::InProcessTransport::pair() };
        icc::Bridge bridge{ bus, context, *endpoints[0] };
        std::optional<pnm::msg::ActionResult<std::uint32_t>> result;
        std::vector<char> events;
        std::vector<std::uint32_t> feedback;
        void SetUp() override
        {
            bridge.registerActions<Actions>(Provider::Local);
            bridge.start();
            drain();
            EXPECT_EQ(receive(icc::Lane::Urgent), frame(13, 0));
            send(frame(13, 0));
            EXPECT_EQ(receive(icc::Lane::Urgent), frame(14, 0));
            send(frame(14, 0));
        }
        void drain()
        {
            while (context.poll()) {
            }
        }
        template<typename T>
        T run(pnm::coro::Task<T> operation)
        {
            auto result = std::make_shared<std::optional<T>>();
            pnm::coro::co_spawn(context, capture(std::move(operation), result));
            drain();
            if (!*result)
                throw std::runtime_error("Expected completed transport operation");
            return std::move(**result);
        }
        void send(const std::vector<std::byte>& bytes)
        {
            const auto kind = std::to_integer<unsigned>(bytes[1]);
            const auto lane = kind >= 11 ? icc::Lane::Urgent : icc::Lane::Normal;
            ASSERT_TRUE(run(endpoints[1]->send(bytes, lane)));
        }
        std::vector<std::byte> receive(icc::Lane lane = icc::Lane::Normal)
        {
            std::vector<std::byte> bytes(2048);
            auto count = run(endpoints[1]->receive(bytes, lane));
            if (!count)
                throw std::runtime_error("Receive failed");
            bytes.resize(*count);
            return bytes;
        }
        auto submit()
        {
            return bus.action<^^Actions::remote>().sendGoal(
              42,
              { .accept_timeout = 2s },
              {
                .on_accepted = [this] { events.push_back('a'); },
                .on_feedback =
                  [this](auto n) {
                events.push_back('f');
                feedback.push_back(n);
            },
                .on_result =
                  [this](auto r) {
                events.push_back('r');
                result.emplace(std::move(r));
            },
              });
        }
        void TearDown() override
        {
            endpoints[0]->close();
            drain();
            bridge.stop();
        }
    };

    TEST_F(ActionWireTest, EncodesGoalAndAcknowledgesIndependentTerminal)
    {
        auto goal = submit();
        drain();
        auto bytes = receive();
        unsigned budget{};
        for (unsigned i = 0; i < 4; ++i)
            budget |= std::to_integer<unsigned>(bytes[12 + i]) << (8 * i);
        EXPECT_GT(budget, 0U);
        EXPECT_LE(budget, 2000U);
        EXPECT_EQ(bytes, frame(6, 1, "remote", value(42), budget));
        send(frame(7, 1));
        send(frame(8, 1, {}, value(12)));
        send(frame(9, 1, {}, terminal(1, 42)));
        ASSERT_TRUE(goal.poll());
        ASSERT_TRUE(result && *result);
        EXPECT_EQ((**result).status, Status::Succeeded);
        EXPECT_EQ((**result).value, 42U);
        EXPECT_EQ(events, (std::vector<char>{ 'a', 'f', 'r' }));
        EXPECT_EQ(receive(icc::Lane::Urgent), frame(12, 1));
        send(frame(9, 1, {}, terminal(1, 42)));
        EXPECT_EQ(receive(icc::Lane::Urgent), frame(12, 1));
        EXPECT_TRUE(goal.poll());
        EXPECT_EQ(events.size(), 3U);
        EXPECT_FALSE(bridge.failure());
    }

    TEST_F(ActionWireTest, EchoesFullIdAndOrdersAcceptanceFeedbackResult)
    {
        constexpr std::uint64_t id = 0x0102030405060708;
        auto provider = bus.action<^^Actions::local>().serveDeferred([](auto n, Execution e) {
            e.accept();
            e.feedback(n - 1);
            e.succeed(n);
        });
        send(frame(6, id, "local", value(23), 1000));
        EXPECT_EQ(provider.poll(), 1U);
        drain();
        EXPECT_EQ(receive(), frame(7, id));
        EXPECT_EQ(receive(), frame(8, id, {}, value(22)));
        EXPECT_EQ(receive(), frame(9, id, {}, terminal(1, 23)));
        send(frame(12, id));
        EXPECT_FALSE(bridge.failure());
    }

    TEST_F(ActionWireTest, RetainsTerminalSlotsUntilAcknowledgedAndRejectsExcess)
    {
        auto provider = bus.action<^^Actions::local>().serveDeferred([](auto n, Execution e) {
            e.accept();
            e.succeed(n);
        });
        for (unsigned id = 1; id <= 8; ++id) {
            send(frame(6, id, "local", value(id), 1000));
            provider.poll();
            drain();
            EXPECT_EQ(receive(), frame(7, id));
            EXPECT_EQ(receive(), frame(9, id, {}, terminal(1, id)));
        }
        send(frame(6, 9, "local", value(9), 1000));
        EXPECT_EQ(provider.poll(), 0U);
        EXPECT_EQ(receive(), frame(10, 9, {}, { std::byte{ 2 } }));
        send(frame(12, 1));
        send(frame(12, 9));
        send(frame(6, 10, "local", value(10), 1000));
        EXPECT_EQ(provider.poll(), 1U);
        drain();
        EXPECT_EQ(receive(), frame(7, 10));
        EXPECT_EQ(receive(), frame(9, 10, {}, terminal(1, 10)));
        send(frame(12, 10));
        EXPECT_FALSE(bridge.failure());
    }

    TEST_F(ActionWireTest, SendsCancelButKeepsGoalPendingUntilRemoteCleanup)
    {
        auto goal = submit();
        drain();
        receive();
        send(frame(7, 1));
        goal.poll();
        goal.requestCancel();
        drain();
        EXPECT_EQ(receive(icc::Lane::Urgent), frame(11, 1));
        EXPECT_FALSE(goal.ready());
        send(frame(9, 1, {}, terminal(3, 7)));
        EXPECT_EQ(receive(icc::Lane::Urgent), frame(12, 1));
        ASSERT_TRUE(goal.poll());
        EXPECT_EQ((**result).status, Status::Cancelled);
    }

    TEST_F(ActionWireTest, MalformedFeedbackFailsCallerAndRequestsRemoteCleanup)
    {
        auto goal = submit();
        drain();
        receive();
        send(frame(7, 1));
        send(frame(8, 1, {}, { std::byte{ 1 } }));
        ASSERT_TRUE(goal.poll());
        EXPECT_EQ(result->error(), Error::TransportError);
        EXPECT_EQ(receive(icc::Lane::Urgent), frame(11, 1));
        send(frame(9, 1, {}, terminal(3, 0)));
        EXPECT_EQ(receive(icc::Lane::Urgent), frame(12, 1));
        EXPECT_FALSE(bridge.failure());
    }

    TEST_F(ActionWireTest, MalformedGoalFailsOnlyItsSubmission)
    {
        auto provider = bus.action<^^Actions::local>().serveDeferred(
          [](auto, Execution) { FAIL() << "Invalid goal dispatched"; });
        send(frame(6, 1, "local", { std::byte{ 1 } }, 1000));
        EXPECT_EQ(provider.poll(), 0U);
        EXPECT_EQ(receive(), frame(10, 1, {}, { std::byte{ 5 } }));
        send(frame(12, 1));
        EXPECT_FALSE(bridge.failure());
    }

    TEST_F(ActionWireTest, EarlyCancellationSuppressesGoalAndRetainsTerminalUntilAcknowledged)
    {
        auto provider = bus.action<^^Actions::local>().serveDeferred(
          [](auto, Execution) { FAIL() << "Cancelled goal ran"; });
        send(frame(11, 42));
        send(frame(11, 42));
        EXPECT_FALSE(bridge.failure());
        send(frame(6, 42, "local", value(42), 1000));
        EXPECT_EQ(provider.poll(), 0U);
        EXPECT_EQ(receive(), frame(9, 42, {}, terminal(3, 0)));
        send(frame(12, 42));
        EXPECT_FALSE(bridge.failure());
    }

    TEST_F(ActionWireTest, BoundsEarlyCancellations)
    {
        for (unsigned id = 1; id <= 8; ++id)
            send(frame(11, id));
        EXPECT_FALSE(bridge.failure());
        send(frame(11, 9));
        EXPECT_TRUE(bridge.failure());
    }

    TEST_F(ActionWireTest, RejectsCancellationOnNormalLane)
    {
        const auto bytes = frame(11, 1);
        ASSERT_TRUE(run(endpoints[1]->send(bytes)));
        EXPECT_TRUE(bridge.failure());
    }

    TEST_F(ActionWireTest, RejectsResultBeforeAcceptance)
    {
        auto goal = submit();
        drain();
        receive();
        send(frame(9, 1, {}, terminal(1, 5)));
        ASSERT_TRUE(goal.poll());
        EXPECT_EQ(result->error(), Error::TransportError);
        EXPECT_TRUE(bridge.failure());
    }

    TEST_F(ActionWireTest, RejectsUnknownStatus)
    {
        auto goal = submit();
        drain();
        receive();
        send(frame(9, 1, {}, terminal(99, 5)));
        ASSERT_TRUE(goal.poll());
        EXPECT_EQ(result->error(), Error::TransportError);
        EXPECT_TRUE(bridge.failure());
    }

    TEST_F(ActionWireTest, RejectsRepeatedGoalAndCancelsLocalExecution)
    {
        std::optional<Execution> execution;
        auto provider = bus.action<^^Actions::local>().serveDeferred([&](auto, Execution e) {
            e.accept();
            execution.emplace(std::move(e));
        });
        send(frame(6, 1, "local", value(1), 1000));
        provider.poll();
        drain();
        receive();
        send(frame(6, 1, "local", value(1), 1000));
        EXPECT_TRUE(bridge.failure());
        EXPECT_TRUE(execution->cancelRequested());
        EXPECT_TRUE(execution->pending());
    }

    TEST_F(ActionWireTest, RejectsAcknowledgementBeforeTerminal)
    {
        std::optional<Execution> execution;
        auto provider = bus.action<^^Actions::local>().serveDeferred(
          [&](auto, Execution e) { execution.emplace(std::move(e)); });
        send(frame(6, 1, "local", value(1), 1000));
        provider.poll();
        send(frame(12, 1));
        EXPECT_TRUE(bridge.failure());
        EXPECT_TRUE(execution->cancelRequested());
    }

    TEST_F(ActionWireTest, RejectsOldProtocolVersion)
    {
        auto bytes = frame(6, 1, "local", value(1), 1000);
        bytes[0] = std::byte{ 3 };
        send(bytes);
        EXPECT_TRUE(bridge.failure());
    }
}
