#pragma once
#include "ITransport.hpp"
#include <array>
#include <memory>

namespace p2p
{
    class InProcessTransport final : public ITransport
    {
      public:
        struct Limits
        {
            std::size_t normal_frames{ 8 }, urgent_frames{ 16 }, frame_bytes{ 2112 }, urgent_bytes{ 512 };
        };
        static std::array<std::shared_ptr<InProcessTransport>, 2> pair();
        static std::array<std::shared_ptr<InProcessTransport>, 2> pair(Limits limits);
        ~InProcessTransport() override;
        SendResult trySend(std::span<const std::byte> frame, Lane lane) override;
        bool tryReceive(Bytes& frame) override;
        bool connected() const override;
        Capacity capacity(Lane lane) const override;
        void setWakeup(std::function<void()> callback) override;
        // Test/application-controlled cable event, affecting both endpoints.
        void setConnected(bool connected);

      private:
        struct Shared;
        InProcessTransport(std::shared_ptr<Shared>, unsigned side);
        std::shared_ptr<Shared> m_shared;
        unsigned m_side;
    };
}
