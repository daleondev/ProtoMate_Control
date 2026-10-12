#include "icc/bridge/Bridge.hpp"
#include "icc/transport/InProcessTransport.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <vector>

namespace
{
    using namespace std::chrono_literals;
    using Error = pnm::msg::ServiceError;

    enum class Provider
    {
        Local,
        Remote
    };
    struct Services
    {
        [[= Provider::Local]] pnm::msg::Service<std::string, std::string> local;
        [[= Provider::Remote]] pnm::msg::Service<std::string, std::string> remote;
        [[= Provider::Remote]] pnm::msg::Service<std::uint32_t, std::uint32_t> number;
    };

    template<typename T>
    pnm::coro::Task<void> capture(pnm::coro::Task<T> operation, std::shared_ptr<std::optional<T>> result)
    {
        result->emplace(co_await operation);
    }

    // Independent peer frames; this test does not use Bridge's encoder.
    std::vector<std::byte> frame(unsigned kind,
                                 std::uint64_t id,
                                 std::string_view name,
                                 std::string_view payload = {},
                                 unsigned timeout = 0)
    {
        std::vector<std::byte> bytes(16 + name.size() + payload.size());
        bytes[0] = std::byte{ 6 };
        bytes[1] = static_cast<std::byte>(kind);
        bytes[2] = static_cast<std::byte>(name.size() & 255);
        bytes[3] = static_cast<std::byte>(name.size() >> 8);
        for (unsigned i = 0; i < 8; ++i)
            bytes[4 + i] = static_cast<std::byte>(id >> (i * 8));
        for (unsigned i = 0; i < 4; ++i)
            bytes[12 + i] = static_cast<std::byte>(timeout >> (i * 8));
        for (std::size_t i = 0; i < name.size(); ++i)
            bytes[16 + i] = static_cast<std::byte>(name[i]);
        for (std::size_t i = 0; i < payload.size(); ++i)
            bytes[16 + name.size() + i] = static_cast<std::byte>(payload[i]);
        return bytes;
    }

    class ServiceWireTest : public ::testing::Test
    {
      protected:
        pnm::coro::Context context;
        static constexpr std::string_view SERVICE{ "local" };
        std::array<std::shared_ptr<icc::InProcessTransport>, 2> endpoints{ icc::InProcessTransport::pair() };
        pnm::msg::Bus bus;
        icc::Bridge bridge{ bus, context, *endpoints[0] };

        void SetUp() override
        {
            bridge.registerServices<Services>(Provider::Local);
            bridge.start();
            drain();
            EXPECT_EQ(receive(icc::Lane::Urgent), frame(13, 0, {}));
            send(frame(13, 0, {}), icc::Lane::Urgent);
            EXPECT_EQ(receive(icc::Lane::Urgent), frame(14, 0, {}));
            send(frame(14, 0, {}), icc::Lane::Urgent);
        }
        void drain()
        {
            while (context.poll() != 0) {
            }
        }
        template<typename T>
        T run(pnm::coro::Task<T> operation)
        {
            auto result = std::make_shared<std::optional<T>>();
            pnm::coro::co_spawn(context, capture(std::move(operation), result));
            drain();
            if (!result->has_value())
                throw std::runtime_error("Expected transport operation to complete");
            return std::move(**result);
        }
        void send(const std::vector<std::byte>& bytes, icc::Lane lane = icc::Lane::Normal)
        {
            ASSERT_TRUE(run(endpoints[1]->send(bytes, lane)));
        }
        std::vector<std::byte> receive(icc::Lane lane = icc::Lane::Normal)
        {
            std::vector<std::byte> bytes(2048);
            auto size = run(endpoints[1]->receive(bytes, lane));
            if (!size)
                throw std::runtime_error("Peer receive failed");
            bytes.resize(*size);
            return bytes;
        }
        void TearDown() override
        {
            endpoints[0]->close();
            drain();
            bridge.stop();
        }
    };

    TEST_F(ServiceWireTest, EncodesRequestAndAcceptsIndependentResponse)
    {
        auto call = bus.service<std::string, std::string>("remote").request("data", 1s);
        drain();
        auto bytes = receive();
        ASSERT_EQ(bytes.size(), 16U + 6U + 4U);
        const auto budget = std::to_integer<unsigned>(bytes[12]) | std::to_integer<unsigned>(bytes[13]) << 8U;
        EXPECT_GT(budget, 0U);
        EXPECT_LE(budget, 1000U);
        EXPECT_EQ(bytes, frame(2, 1, "remote", "data", budget));
        send(frame(3, 1, {}, "response"));
        ASSERT_TRUE(call.ready());
        EXPECT_EQ(call.get(), "response");
    }

    TEST_F(ServiceWireTest, EchoesFullLittleEndianCallIdAndRejectsUnknownService)
    {
        constexpr auto id = UINT64_C(0x0102030405060708);
        send(frame(2, id, "absent", {}, 1000));
        EXPECT_EQ(receive(), frame(4, id, {}, std::string(1, '\1'))); // Unavailable
        EXPECT_FALSE(bridge.failure());
    }

    TEST_F(ServiceWireTest, RejectsDuplicateRequestWithoutExecutingHandlerTwice)
    {
        auto server =
          bus.service<std::string, std::string>(SERVICE).serve([](const auto& value) { return value; });
        send(frame(2, 42, SERVICE, "once", 1000));
        EXPECT_EQ(server.poll(), 1U);
        drain();
        EXPECT_EQ(receive(), frame(3, 42, {}, "once"));
        send(frame(2, 42, SERVICE, "again", 1000));
        EXPECT_TRUE(bridge.failure());
        EXPECT_EQ(server.poll(), 0U);
    }

    TEST_F(ServiceWireTest, BoundsIncomingCallsAndReturnsBusy)
    {
        auto server =
          bus.service<std::string, std::string>(SERVICE).serve([](const auto& value) { return value; });
        for (unsigned i = 1; i <= 8; ++i)
            send(frame(2, i, SERVICE, {}, 1000));
        send(frame(2, 9, SERVICE, {}, 1000));
        EXPECT_EQ(receive(), frame(4, 9, {}, std::string(1, '\2'))); // Busy
        EXPECT_EQ(server.poll(), 8U);
        drain();
        EXPECT_FALSE(bridge.failure());
    }

    TEST_F(ServiceWireTest, RetainsCompletedRepliesWhileTransportIsFull)
    {
        auto server =
          bus.service<std::string, std::string>(SERVICE).serve([](const auto& value) { return value; });
        for (unsigned batch = 0; batch < 2; ++batch) {
            for (unsigned i = 1; i <= 8; ++i) {
                const auto id = batch * 8 + i;
                send(frame(2, id, SERVICE, std::to_string(id), 1000));
            }
            ASSERT_EQ(server.poll(), 8U);
            drain(); // Second batch completes with the transport still full.
        }
        for (unsigned id = 1; id <= 16; ++id)
            EXPECT_EQ(receive(), frame(3, id, {}, std::to_string(id)));
        EXPECT_FALSE(bridge.failure());
    }

    TEST_F(ServiceWireTest, InvalidPayloadFailsOnlyItsCall)
    {
        auto call = bus.service<std::uint32_t, std::uint32_t>("number").request(123, 1s);
        drain();
        receive();
        send(frame(3, 1, {}, "x")); // uint32_t needs exactly four bytes.
        ASSERT_TRUE(call.ready());
        EXPECT_EQ(call.get(), std::unexpected(Error::TransportError));
        EXPECT_FALSE(bridge.failure());
    }

    TEST_F(ServiceWireTest, EarlyUrgentCancellationSuppressesTheLaterRequest)
    {
        auto provider = bus.service<^^Services::local>().serve([](const auto& value) { return value; });
        send(frame(5, 42, {}, std::string(1, '\0')), icc::Lane::Urgent);
        send(frame(5, 42, {}, std::string(1, '\0')), icc::Lane::Urgent); // Idempotent.
        EXPECT_FALSE(bridge.failure());
        send(frame(2, 42, "local", "must not run", 1000));
        EXPECT_EQ(provider.poll(), 0U);
        EXPECT_EQ(receive(), frame(4, 42, {}, std::string(1, '\4')));
        send(frame(2, 43, "local", "next", 1000));
        EXPECT_EQ(provider.poll(), 1U);
        drain();
        EXPECT_EQ(receive(), frame(3, 43, {}, "next"));
        EXPECT_FALSE(bridge.failure());
    }

    TEST_F(ServiceWireTest, BoundsCancellationsThatOvertakeRequests)
    {
        for (unsigned id = 1; id <= 8; ++id)
            send(frame(5, id, {}, std::string(1, '\0')), icc::Lane::Urgent);
        EXPECT_FALSE(bridge.failure());
        send(frame(5, 9, {}, std::string(1, '\0')), icc::Lane::Urgent);
        EXPECT_TRUE(bridge.failure());
    }

    TEST_F(ServiceWireTest, RejectsNormalServiceRequestsOnUrgentLane)
    {
        auto provider = bus.service<^^Services::local>().serve([](const auto& value) { return value; });
        send(frame(2, 1, "local", "wrong lane", 1000), icc::Lane::Urgent);
        EXPECT_TRUE(bridge.failure());
        EXPECT_EQ(provider.poll(), 0U);
    }

    TEST_F(ServiceWireTest, RejectsCancellationOnNormalLane)
    {
        send(frame(5, 1, {}, std::string(1, '\0')));
        EXPECT_TRUE(bridge.failure());
    }

    TEST_F(ServiceWireTest, InvalidEnvelopeFailsConnectionAndOutstandingCalls)
    {
        auto call = bus.service<std::string, std::string>("remote").request("waiting", 1s);
        drain();
        auto invalid = frame(3, 1, {}, "response");
        invalid[2] = std::byte{ 255 }; // Declared name length exceeds the entire frame.
        send(invalid);
        EXPECT_TRUE(bridge.failure());
        ASSERT_TRUE(call.ready());
        EXPECT_EQ(call.get(), std::unexpected(Error::TransportError));
    }

    TEST_F(ServiceWireTest, InvalidErrorCodeFailsConnection)
    {
        auto call = bus.service<std::string, std::string>("remote").request("waiting", 1s);
        drain();
        send(frame(4, 1, {}, std::string(1, '\x7f')));
        EXPECT_TRUE(bridge.failure());
        ASSERT_TRUE(call.ready());
        EXPECT_EQ(call.get(), std::unexpected(Error::TransportError));
    }
}
