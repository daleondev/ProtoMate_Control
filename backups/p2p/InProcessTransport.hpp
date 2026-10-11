#pragma once
#include "ITransport.hpp"
#include <array>
#include <memory>

namespace p2p
{
    class InProcessTransport final : public ITransport
    {
      public:
        static constexpr std::size_t DEFAULT_NORMAL_FRAMES = 8;
        static constexpr std::size_t DEFAULT_URGENT_FRAMES = 16;
        static constexpr std::size_t DEFAULT_FRAME_BYTES = 2112;
        static constexpr std::size_t DEFAULT_URGENT_BYTES = 512;
        struct Limits
        {
            std::size_t normal_frames{ DEFAULT_NORMAL_FRAMES };
            std::size_t urgent_frames{ DEFAULT_URGENT_FRAMES };
            std::size_t frame_bytes{ DEFAULT_FRAME_BYTES };
            std::size_t urgent_bytes{ DEFAULT_URGENT_BYTES };
        };
        static std::array<std::shared_ptr<InProcessTransport>, 2> pair();
        static std::array<std::shared_ptr<InProcessTransport>, 2> pair(Limits limits);
        ~InProcessTransport() override;
        InProcessTransport(const InProcessTransport&) = delete;
        InProcessTransport& operator=(const InProcessTransport&) = delete;
        InProcessTransport(InProcessTransport&&) = delete;
        InProcessTransport& operator=(InProcessTransport&&) = delete;
        SendResult trySend(std::span<const std::byte> frame, Lane lane) override;
        bool tryReceive(Bytes& frame) override;
        bool connected() const override;
        Capacity capacity(Lane lane) const override;
        void setWakeup(std::function<void()> callback) override;
        // Test/application-controlled cable event, affecting both endpoints.
        void setConnected(bool connected);

      private:
        struct Shared;
        InProcessTransport(std::shared_ptr<Shared> shared, unsigned side);
        std::shared_ptr<Shared> m_shared;
        unsigned m_side;
    };
}
