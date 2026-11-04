#pragma once
#include "common/icc/transport/ITransport.hpp"
#include "simulation/SharedMemory.hpp"
namespace simulation
{
    // Dedicated ICC link. The register mailbox has separate capacity and never
    // requires the ThreadX context to run while a HAL transaction holds logical time.
    class SharedTransport final : public icc::ITransport
    {
      public:
        SharedTransport(SharedMemory& memory, unsigned side, pnm::coro::Context& context)
          : m_memory{ memory }
          , m_side{ side }
          , m_context{ context }
        {
            if (side > 1)
                throw std::invalid_argument{ "invalid simulation transport side" };
        }
        pnm::coro::Task<pnm::EmptyResult<Error>> send(std::span<const std::byte> frame,
                                                      icc::Lane lane,
                                                      std::stop_token stop) override
        {
            if (static_cast<unsigned>(lane) >= 3)
                co_return std::unexpected(Error::Disconnected);
            if (frame.size() >
                (lane == icc::Lane::Background ? icc::MAX_BACKGROUND_FRAME_BYTES : FRAME_BYTES))
                co_return std::unexpected(Error::TooLarge);
            for (;;) {
                if (stop.stop_requested())
                    co_return std::unexpected(Error::Cancelled);
                if (acquire(m_memory.closed))
                    co_return std::unexpected(Error::Disconnected);
                auto result{ enqueue(m_memory.incoming[1 - m_side][static_cast<unsigned>(lane)], frame) };
                if (result == QueueResult::Success)
                    co_return pnm::EmptyResult<Error>{};
                if (result != QueueResult::Full)
                    co_return std::unexpected(Error::Disconnected);
                co_await pnm::coro::sleep(m_context, std::chrono::milliseconds{ 1 }, stop);
            }
        }
        pnm::coro::Task<pnm::Result<std::size_t, Error>> receive(std::span<std::byte> buffer,
                                                                 icc::Lane lane,
                                                                 std::stop_token stop) override
        {
            if (static_cast<unsigned>(lane) >= 3)
                co_return std::unexpected(Error::Disconnected);
            for (;;) {
                if (stop.stop_requested())
                    co_return std::unexpected(Error::Cancelled);
                if (acquire(m_memory.closed))
                    co_return std::unexpected(Error::Disconnected);
                bool higher{};
                for (auto priority : icc::LANE_PRIORITY) {
                    if (priority == lane)
                        break;
                    auto& q{ m_memory.incoming[m_side][static_cast<unsigned>(priority)] };
                    higher |= acquire(q.written) != acquire(q.read);
                }
                if (!higher) {
                    std::size_t size{};
                    auto result{ dequeue(
                      m_memory.incoming[m_side][static_cast<unsigned>(lane)], buffer, size) };
                    if (result == QueueResult::Success)
                        co_return size;
                    if (result == QueueResult::BufferTooSmall)
                        co_return std::unexpected(Error::BufferTooSmall);
                    if (result != QueueResult::Empty)
                        co_return std::unexpected(Error::Disconnected);
                }
                co_await pnm::coro::sleep(m_context, std::chrono::milliseconds{ 1 }, stop);
            }
        }

      private:
        SharedMemory& m_memory;
        unsigned m_side;
        pnm::coro::Context& m_context;
    };
}
