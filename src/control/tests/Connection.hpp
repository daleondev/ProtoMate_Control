// Numeric values describe explicit test inputs, timing bounds and expected results.
// NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
#pragma once
#include "control/ControllerClient.hpp"
#include "control/ControllerServer.hpp"
#include "p2p/InProcessTransport.hpp"
#include "runtime/thread.hpp"

namespace control::test
{
    // Integration fixture: two real buses, two contexts, wire codecs and the IO
    // command worker. Only hardware is simulated, never the execution boundary.
    class Connection
    {
      public:
        pnm::msg::Bus ui_bus, io_bus;
        pnm::coro::Context ui_context, io_context;
        std::array<std::shared_ptr<p2p::InProcessTransport>, 2> transport = p2p::InProcessTransport::pair();
        p2p::Bridge ui_bridge{ ui_bus, ui_context, transport[0] },
          io_bridge{ io_bus, io_context, transport[1] };
        std::shared_ptr<ControllerClient> client;
        std::unique_ptr<ControllerServer> server;
        std::jthread ui_thread, io_thread;
        explicit Connection(std::shared_ptr<MotionController> controller)
        {
            server = std::make_unique<ControllerServer>(std::move(controller), io_bus, io_context);
            protocol::routes(ui_bridge, false);
            protocol::routes(io_bridge, true);
            io_bridge.onStateChanged([this](auto state) {
                if (server)
                    server->linkChanged(state);
            });
            client = std::make_shared<ControllerClient>(ui_bus, ui_context);
            ui_bridge.start();
            io_bridge.start();
            ui_thread = runtime::thread::create_jthread(
              { .name = "test UI bus", .priority = 4, .stack_size = 65536U }, [this] { ui_context.run(); });
            io_thread = runtime::thread::create_jthread(
              { .name = "test IO bus", .priority = 3, .stack_size = 65536U }, [this] { io_context.run(); });
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (ui_bridge.state() != p2p::LinkState::Connected ||
                   io_bridge.state() != p2p::LinkState::Connected) {
                if (std::chrono::steady_clock::now() >= deadline)
                    throw std::runtime_error("test connection handshake timed out");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;
        Connection(Connection&&) = delete;
        Connection& operator=(Connection&&) = delete;
        ~Connection()
        {
            client = nullptr;
            ui_bridge.close();
            io_bridge.close();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (
              (ui_bridge.state() != p2p::LinkState::Closed || io_bridge.state() != p2p::LinkState::Closed) &&
              std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            server.reset();
            ui_context.stop();
            io_context.stop();
            if (ui_thread.joinable())
                ui_thread.join();
            if (io_thread.joinable())
                io_thread.join();
        }
    };
}

// NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
