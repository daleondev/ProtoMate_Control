#pragma once

#include "hal/drivers/itf/IInterCoreChannel.hpp"
#include "hal/util/InterCoreMemory.hpp"

namespace hal
{
    class InterCoreChannel final : public IInterCoreChannel
    {
      public:
        InterCoreChannel();
        ~InterCoreChannel() override;
        std::expected<void, Error> trySend(std::span<const std::byte> frame, Lane lane) noexcept override;
        std::expected<std::size_t, Error> tryReceive(std::span<std::byte> buffer, Lane lane) noexcept override;
        void setNotification(Notification callback) noexcept override;
        void close() noexcept override;

        // M7 calls once before releasing M4 from the boot handshake.
        static void initializeMemory() noexcept;
        static void dispatchNotification(std::uint32_t mask) noexcept;

      private:
        bool disconnected() const noexcept;
        static void notifyPeer() noexcept;
        Notification m_notification;
        inline static InterCoreChannel* s_instance{};
    };
}
