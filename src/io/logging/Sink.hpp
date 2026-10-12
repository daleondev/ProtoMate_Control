#pragma once

#include <pneumo/logging.hpp>
#include <pneumo/messaging.hpp>

namespace io::logging
{
    // Accepts already formatted Pneumo records. Publishing only enqueues local
    // messages: it never waits for the peer, transport space or console output.
    // Use from thread context, not interrupts. close() permanently detaches it;
    // queued Pneumo entries may retain the sink after its application is gone.
    class Sink final : public pnm::log::SinkBase<Sink>
    {
      public:
        explicit Sink(pnm::msg::Bus& bus);
        bool isOpen() const override;
        pnm::Result<> open() override;
        pnm::Result<> close() override;
        pnm::Result<std::size_t> write(std::span<const std::byte> data) override;
        pnm::Result<> flush() override;
        std::size_t truncatedRecords() const noexcept;

      private:
        mutable std::mutex m_mutex;
        std::optional<pnm::msg::Topic<std::string>> m_topic;
        std::atomic_size_t m_truncated{};
    };
}
