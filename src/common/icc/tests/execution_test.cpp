#include "common/icc/bridge/Bridge.hpp"
#include "common/icc/transport/InProcessTransport.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <exception>
#include <optional>
#include <stop_token>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
    namespace ex = pnm::coro::execution;
    using namespace std::chrono_literals;
    using Result = pnm::msg::ServiceResult<std::string>;
    using Reply = pnm::msg::Reply<std::string>;

    enum class Provider { IO, UI };
    struct Services
    {
        [[= Provider::IO]] pnm::msg::Service<std::string, std::string> echo;
    };

    struct Completion
    {
        std::optional<Result> result;
        std::exception_ptr error;
        unsigned stopped{};
        unsigned deliveries{};
    };

    struct Receiver
    {
        using receiver_concept = ex::receiver_tag;
        Completion* completion;
        std::stop_token stop;
        auto get_env() const noexcept { return ex::prop{ ex::get_stop_token, stop }; }
        void set_value(Result value) && noexcept
        {
            completion->result.emplace(std::move(value));
            ++completion->deliveries;
        }
        void set_error(std::exception_ptr error) && noexcept
        {
            completion->error = std::move(error);
            ++completion->deliveries;
        }
        void set_stopped() && noexcept
        {
            ++completion->stopped;
            ++completion->deliveries;
        }
    };

    // The test executable uses our ThreadX/libstdc++ runtime. Each request still
    // crosses the serialized bridge; stdexec composes local operations only.
    class ExecutionTest : public ::testing::Test
    {
      protected:
        pnm::msg::Bus ioBus, uiBus;
        pnm::coro::Context ioContext, uiContext;
        std::array<std::shared_ptr<icc::InProcessTransport>, 2> endpoints{ icc::InProcessTransport::pair() };
        icc::Bridge io{ ioBus, ioContext, *endpoints[0] }, ui{ uiBus, uiContext, *endpoints[1] };
        pnm::coro::Bus client{ uiContext, uiBus };
        std::vector<Reply> replies;
        std::optional<std::tuple<Result, Result>> results;
        std::exception_ptr error;

        void SetUp() override
        {
            io.registerServices<Services>(Provider::IO);
            ui.registerServices<Services>(Provider::UI);
            io.start();
            ui.start();
            drain();
        }
        void drain()
        {
            for (unsigned turn{}; turn < 1000; ++turn)
                if (ioContext.poll() + uiContext.poll() == 0)
                    return;
            FAIL() << "ICC contexts did not become idle";
        }
        void TearDown() override
        {
            client.requestStop();
            endpoints[0]->close();
            drain();
            io.stop();
            ui.stop();
        }
        auto serve()
        {
            return ioBus.service<^^Services::echo>().serveDeferred(
              [this](const std::string&, Reply reply) { replies.push_back(std::move(reply)); });
        }
        auto request(std::string value)
        {
            return pnm::coro::asSender(
              uiContext, [service = client.service<^^Services::echo>(), value = std::move(value)](
                           std::stop_token stop) { return service.request(value, 1s, stop); });
        }
    };

    TEST_F(ExecutionTest, WhenAllCorrelatesOutOfOrderRemoteReplies)
    {
        auto provider{ serve() };
        pnm::coro::co_spawn(uiContext, [this](auto&) -> pnm::coro::Task<void> {
            try {
                results = co_await ex::when_all(request("first"), request("second"));
            } catch (...) {
                error = std::current_exception();
            }
        });
        drain();
        ASSERT_EQ(provider.poll(), 2U);
        ASSERT_EQ(replies.size(), 2U);
        replies[1].respond("second response");
        drain();
        EXPECT_FALSE(results);
        replies[0].respond("first response");
        drain();
        ASSERT_FALSE(error);
        ASSERT_TRUE(results);
        EXPECT_EQ(std::get<0>(*results), "first response");
        EXPECT_EQ(std::get<1>(*results), "second response");
    }

    TEST_F(ExecutionTest, BusyIsADomainResultAndDoesNotCancelTheOtherRequest)
    {
        auto provider{ serve() };
        pnm::coro::co_spawn(uiContext, [this](auto&) -> pnm::coro::Task<void> {
            try {
                results = co_await ex::when_all(request("busy"), request("accepted"));
            } catch (...) {
                error = std::current_exception();
            }
        });
        drain();
        ASSERT_EQ(provider.poll(), 2U);
        ASSERT_EQ(replies.size(), 2U);
        replies[0].fail(pnm::msg::ServiceError::Busy);
        drain();
        EXPECT_FALSE(results);
        EXPECT_TRUE(replies[1].pending());
        replies[1].respond("completed");
        drain();
        ASSERT_FALSE(error);
        ASSERT_TRUE(results);
        EXPECT_EQ(std::get<0>(*results), std::unexpected(pnm::msg::ServiceError::Busy));
        EXPECT_EQ(std::get<1>(*results), "completed");
    }

    TEST_F(ExecutionTest, ReceiverCancellationReachesRemoteProviderExactlyOnce)
    {
        auto provider{ serve() };
        std::stop_source stop;
        Completion completion;
        auto operation{ ex::connect(request("cancel"), Receiver{ &completion, stop.get_token() }) };
        ex::start(operation);
        drain();
        ASSERT_EQ(provider.poll(), 1U);
        ASSERT_EQ(replies.size(), 1U);
        EXPECT_TRUE(replies[0].pending());
        stop.request_stop();
        drain();
        EXPECT_FALSE(replies[0].pending());
        ASSERT_TRUE(completion.result);
        EXPECT_EQ(*completion.result, std::unexpected(pnm::msg::ServiceError::Cancelled));
        EXPECT_FALSE(completion.error);
        EXPECT_EQ(completion.stopped, 0U);
        EXPECT_EQ(completion.deliveries, 1U);
        replies[0].respond("late response");
        drain();
        EXPECT_EQ(completion.deliveries, 1U);
        EXPECT_FALSE(io.failure());
        EXPECT_FALSE(ui.failure());
    }
}
