#include "common/diagnostics/Memory.hpp"
#include "common/icc/bridge/Bridge.hpp"
#include "common/icc/bridge/Frame.hpp"
#include "common/icc/catalog/services.hpp"
#include "common/icc/examples/ContextThread.hpp"
#include "common/icc/transport/InProcessTransport.hpp"
#include "io/diagnostics/Server.hpp"
#include "ui/cli/Parser.hpp"
#include "ui/cli/utilities.hpp"
#include "runtime/thread.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <sstream>
#include <string>

namespace
{
    using namespace std::chrono_literals;
    using diagnostics::MemoryQuery;
    using diagnostics::MemorySnapshot;

    struct MemoryServiceTest : ::testing::Test
    {
        pnm::msg::Bus ioBus, uiBus;
        pnm::coro::Context ioContext, uiContext;
        decltype(icc::InProcessTransport::pair()) endpoints{ icc::InProcessTransport::pair() };
        std::jthread ioRunner{ example::run_context(ioContext) };
        std::jthread uiRunner{ example::run_context(uiContext) };
        icc::Bridge io{ ioBus, ioContext, *endpoints[0] }, ui{ uiBus, uiContext, *endpoints[1] };
        std::stop_source stop;
        ui::cli::Parser parser;

        void SetUp() override
        {
            io.registerServices<icc::Services>(icc::ServiceProvider::IO);
            ui.registerServices<icc::Services>(icc::ServiceProvider::UI);
            ui::cli::utilities::setup(parser, uiBus, stop.get_token());
            io.start();
            ui.start();
        }

        void TearDown() override
        {
            stop.request_stop();
            io.stop();
            ui.stop();
        }
    };

    TEST_F(MemoryServiceTest, ControllerArgumentIsRequiredAndValidated)
    {
        std::ostringstream output;
        for (const auto command : { "heap", "stack", "heap ui io", "stack io extra" }) {
            EXPECT_EQ(parser.execute(command, output).error, ui::cli::ExecutionError::InvalidArguments);
        }
        for (const auto command : { "heap m4", "stack unknown" }) {
            const auto result{ parser.execute(command, output) };
            EXPECT_EQ(result.error, ui::cli::ExecutionError::CallbackFailed);
            EXPECT_NE(result.message.find("'ui' or 'io'"), std::string::npos);
        }
        EXPECT_NE(parser.usage("heap").find("controller"), std::string::npos);
        EXPECT_NE(parser.help("stack").find("via ICC"), std::string::npos);
    }

    TEST_F(MemoryServiceTest, LocalCommandsWorkWithoutRemoteProviderAndDescribeHostScope)
    {
        for (const auto command : { "heap ui", "stack ui", "time 'heap ui'" }) {
            std::ostringstream output;
            const auto result{ parser.execute(command, output) };
            ASSERT_TRUE(result) << result.message;
            EXPECT_NE(output.str().find("Controller: UI"), std::string::npos);
            EXPECT_NE(output.str().find("Linux: shared process"), std::string::npos);
        }
        std::ostringstream output;
        const auto result{ parser.execute("heap io", output) };
        EXPECT_EQ(result.error, ui::cli::ExecutionError::CallbackFailed);
        EXPECT_NE(result.message.find("unavailable"), std::string::npos);
        EXPECT_TRUE(output.str().empty()); // Never fall back to UI data.
    }

    TEST_F(MemoryServiceTest, RealIoProviderReturnsHeapAndOwnedThreadSnapshotsAcrossBridge)
    {
        io::diagnostics::Server server{ ioBus };
        auto worker{ runtime::thread::create_jthread(
          { .name = "io.diag.test", .stack_size = 16384 },
          [&server](std::stop_token token) { server.run(token); }) };
        const std::string worker_name{ worker.native_handle()->tx_thread_name };
        for (const auto command : { "heap io", "stack io" }) {
            std::ostringstream output;
            const auto result{ parser.execute(command, output) };
            ASSERT_TRUE(result) << result.message;
            EXPECT_NE(output.str().find("Controller: IO"), std::string::npos);
            EXPECT_NE(output.str().find("Linux: shared process"), std::string::npos);
            if (std::string_view{ command }.starts_with("stack")) {
                EXPECT_NE(output.str().find(worker_name), std::string::npos) << output.str();
                EXPECT_NE(output.str().find("Stack-Usage Bar"), std::string::npos);
            }
            else { EXPECT_NE(output.str().find("glibc"), std::string::npos); }
        }
        worker.request_stop();
        worker.join();
        EXPECT_FALSE(io.failure());
        EXPECT_FALSE(ui.failure());
    }

    TEST_F(MemoryServiceTest, DisplaysRemoteValuesAndReportsBoundsInsteadOfReadingLocalState)
    {
        auto server{ ioBus.service<^^icc::Services::io_memory>().serve([](MemoryQuery query) {
            MemorySnapshot result{};
            result.query = query;
            result.heap.capacity = 424242;
            result.stack.count = 1;
            result.stack.total = 40;
            std::ranges::copy(std::string_view{ "remote-only-thread" }, result.stack.threads[0].name.begin());
            // A zero-sized stack must not divide by zero during rendering.
            return result;
        }) };
        for (const auto command : { "heap io", "stack io" }) {
            std::ostringstream output;
            ui::cli::ExecutionResult result;
            auto caller{ runtime::thread::create({ .stack_size = 32768 }, [&] {
                result = parser.execute(command, output);
            }) };
            EXPECT_EQ(server.poll(1s), 1U);
            caller.join();
            ASSERT_TRUE(result) << result.message;
            if (std::string_view{ command }.starts_with("heap")) {
                EXPECT_NE(output.str().find("424242"), std::string::npos);
                EXPECT_NE(output.str().find("Newlib"), std::string::npos);
            }
            else {
                EXPECT_NE(output.str().find("remote-only-thread"), std::string::npos);
                EXPECT_NE(output.str().find("1 of 40"), std::string::npos);
            }
            EXPECT_EQ(output.str().find("Linux:"), std::string::npos);
        }
    }

    TEST_F(MemoryServiceTest, ShutdownCancelsPendingCommandWithoutWaitingForTimeout)
    {
        std::optional<pnm::msg::Reply<MemorySnapshot>> pending;
        auto server{ ioBus.service<^^icc::Services::io_memory>().serveDeferred(
          [&](MemoryQuery, pnm::msg::Reply<MemorySnapshot> reply) { pending.emplace(std::move(reply)); }) };
        std::ostringstream output;
        ui::cli::ExecutionResult result;
        auto caller{ runtime::thread::create({ .stack_size = 32768 }, [&] {
            result = parser.execute("stack io", output);
        }) };
        EXPECT_EQ(server.poll(1s), 1U);
        const auto before{ std::chrono::steady_clock::now() };
        stop.request_stop();
        caller.join();
        EXPECT_LT(std::chrono::steady_clock::now() - before, 500ms);
        EXPECT_EQ(result.error, ui::cli::ExecutionError::CallbackFailed);
        EXPECT_NE(result.message.find("cancelled"), std::string::npos);
    }

    TEST_F(MemoryServiceTest, RejectsMalformedRemoteSnapshot)
    {
        auto server{ ioBus.service<^^icc::Services::io_memory>().serve([](MemoryQuery query) {
            MemorySnapshot result{};
            result.query = query;
            result.stack.count = 1000;
            result.stack.total = 1000;
            return result;
        }) };
        std::ostringstream output;
        ui::cli::ExecutionResult result;
        auto caller{ runtime::thread::create({ .stack_size = 32768 }, [&] {
            result = parser.execute("stack io", output);
        }) };
        EXPECT_EQ(server.poll(1s), 1U);
        caller.join();
        EXPECT_EQ(result.error, ui::cli::ExecutionError::CallbackFailed);
        EXPECT_NE(result.message.find("invalid response"), std::string::npos);
        EXPECT_TRUE(output.str().empty());
    }

    TEST_F(MemoryServiceTest, ReportsBusyService)
    {
        auto server{ ioBus.service<^^icc::Services::io_memory>().serveDeferred(
          [](MemoryQuery, pnm::msg::Reply<MemorySnapshot> reply) {
              reply.fail(pnm::msg::ServiceError::Busy);
          }) };
        std::ostringstream output;
        ui::cli::ExecutionResult result;
        auto caller{ runtime::thread::create({ .stack_size = 32768 }, [&] {
            result = parser.execute("heap io", output);
        }) };
        EXPECT_EQ(server.poll(1s), 1U);
        caller.join();
        EXPECT_EQ(result.error, ui::cli::ExecutionError::CallbackFailed);
        EXPECT_NE(result.message.find("busy"), std::string::npos);
    }

    TEST_F(MemoryServiceTest, TimeoutAndDisconnectReturnErrorsWithoutLocalFallback)
    {
        auto server{ ioBus.service<^^icc::Services::io_memory>().serveDeferred(
          [](MemoryQuery, pnm::msg::Reply<MemorySnapshot>) {}) };
        std::ostringstream output;
        auto result{ parser.execute("heap io", output) }; // No dispatch: bounded timeout.
        EXPECT_EQ(result.error, ui::cli::ExecutionError::CallbackFailed);
        EXPECT_NE(result.message.find("timed out"), std::string::npos);
        endpoints[0]->close();
        result = parser.execute("stack io", output);
        EXPECT_EQ(result.error, ui::cli::ExecutionError::CallbackFailed);
        EXPECT_TRUE(output.str().empty());
    }

    TEST(MemoryDiagnostics, SnapshotFitsWireFrameAndCollectionHasSaneBounds)
    {
        for (auto query : { MemoryQuery::Heap, MemoryQuery::Stack }) {
            const auto snapshot{ diagnostics::collectMemory(query) };
            const auto frame{ icc::util::encode_frame(icc::util::FrameKind::Response, "io/memory", 1, snapshot) };
            EXPECT_LE(frame.size, icc::util::MAX_FRAME_BYTES);
            MemorySnapshot decoded{};
            ASSERT_TRUE(pnm::utils::memory::deserialize(icc::util::decode_frame(
              std::span{ frame.bytes }.first(frame.size)).payload, decoded));
            EXPECT_EQ(decoded.query, query);
            EXPECT_TRUE(decoded.shared_runtime);
            EXPECT_LE(decoded.stack.count, decoded.stack.threads.size());
            EXPECT_LE(decoded.stack.count, decoded.stack.total);
            for (const auto& thread : std::span{ decoded.stack.threads }.first(decoded.stack.count)) {
                EXPECT_EQ(thread.name.back(), '\0');
                EXPECT_LE(thread.current_stack_usage, thread.peak_stack_usage);
                EXPECT_LE(thread.peak_stack_usage, thread.stack_size);
            }
        }
        EXPECT_THROW(static_cast<void>(diagnostics::collectMemory(static_cast<MemoryQuery>(99))),
                     std::invalid_argument);
    }
}
