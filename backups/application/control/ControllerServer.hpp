#pragma once
#include "MotionController.hpp"
#include "Messages.hpp"
#include "p2p/bridge/ConnectionHealth.hpp"

namespace control
{
    class ControllerServer final
    {
      public:
        ControllerServer(std::shared_ptr<MotionController> controller,
                         pnm::msg::Bus& bus,
                         pnm::coro::Context& context);
        ~ControllerServer();
        ControllerServer(const ControllerServer&) = delete;
        ControllerServer& operator=(const ControllerServer&) = delete;
        ControllerServer(ControllerServer&&) = delete;
        ControllerServer& operator=(ControllerServer&&) = delete;
        // Safe to call from the bridge's context; joins run on the IO worker.
        void linkChanged(p2p::ConnectionStatus status);

      private:
        struct State;
        std::shared_ptr<State> m_state;
    };
}
