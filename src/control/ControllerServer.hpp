#pragma once
#include "MotionController.hpp"
#include "Protocol.hpp"

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
        void linkChanged(p2p::LinkState state);

      private:
        struct State;
        std::shared_ptr<State> m_state;
    };
}
