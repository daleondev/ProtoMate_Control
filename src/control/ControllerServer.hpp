#pragma once
#include "MotionController.hpp"
#include "Protocol.hpp"

namespace control
{
    class ControllerServer final
    {
      public:
        ControllerServer(std::shared_ptr<MotionController>, pnm::msg::Bus&, pnm::coro::Context&);
        ~ControllerServer();
        ControllerServer(const ControllerServer&) = delete;
        ControllerServer& operator=(const ControllerServer&) = delete;
        void linkChanged(p2p::LinkState);

      private:
        struct State;
        std::shared_ptr<State> m_state;
    };
}
