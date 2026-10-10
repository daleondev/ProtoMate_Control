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
        pnm::msg::Bus uiBus, ioBus;
        pnm::coro::Context uiContext, ioContext;
        std::array<std::shared_ptr<p2p::InProcessTransport>, 2> transport = p2p::InProcessTransport::pair();
        p2p::Bridge uiBridge{ uiBus, uiContext, transport[0] }, ioBridge{ ioBus, ioContext, transport[1] };
        std::shared_ptr<ControllerClient> client;
        std::unique_ptr<ControllerServer> server;
        std::jthread uiThread, ioThread;
        explicit Connection(std::shared_ptr<MotionController> controller)
        {
            server = std::make_unique<ControllerServer>(std::move(controller), ioBus, ioContext);
            protocol::routes(uiBridge, false);
            protocol::routes(ioBridge, true);
            ioBridge.onStateChanged([this](auto state) {
                if (server)
                    server->linkChanged(state);
            });
            client = std::make_shared<ControllerClient>(uiBus, uiContext);
            uiBridge.start();
            ioBridge.start();
            uiThread = runtime::thread::create_jthread(
              { .name = "test UI bus", .priority = 4, .stack_size = 65536U }, [this] { uiContext.run(); });
            ioThread = runtime::thread::create_jthread(
              { .name = "test IO bus", .priority = 3, .stack_size = 65536U }, [this] { ioContext.run(); });
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (uiBridge.state() != p2p::LinkState::Connected ||
                   ioBridge.state() != p2p::LinkState::Connected) {
                if (std::chrono::steady_clock::now() >= deadline)
                    throw std::runtime_error("test connection handshake timed out");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        ~Connection()
        {
            client.reset();
            uiBridge.close();
            ioBridge.close();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            while (
              (uiBridge.state() != p2p::LinkState::Closed || ioBridge.state() != p2p::LinkState::Closed) &&
              std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            server.reset();
            uiContext.stop();
            ioContext.stop();
            if (uiThread.joinable())
                uiThread.join();
            if (ioThread.joinable())
                ioThread.join();
        }
    };
}
