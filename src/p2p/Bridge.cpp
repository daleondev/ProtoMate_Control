#include "Bridge.hpp"

#include <stdexcept>

namespace p2p
{
    Bridge::Bridge(pnm::msg::Bus& bus, pnm::coro::Context& context, ITransport& transport)
      : m_bus(bus)
      , m_context(context)
      , m_transport(transport)
    {
    }

    pnm::coro::Task<pnm::EmptyResult<ITransport::Error>> Bridge::sendLoop(std::stop_token stop)
    {
        while (!stop.stop_requested()) {
            // Suspends until a local publication provides a frame or we stop.
            auto frame = co_await nextOutgoingFrame(stop);
            if (!frame || stop.stop_requested())
                break;

            if (frame->size > frame->bytes.size())
                throw std::length_error("Outgoing bridge frame exceeds its buffer");

            // frame owns the storage throughout the await. A full transport
            // suspends this loop while the receive loop can continue running.
            const auto bytes = std::span<const std::byte>(frame->bytes).first(frame->size);
            auto result = co_await m_transport.send(bytes, stop);
            if (!result) {
                if (result.error() == ITransport::Error::Cancelled && stop.stop_requested())
                    break;
                co_return std::unexpected(result.error());
            }
        }
        co_return pnm::EmptyResult<ITransport::Error>{};
    }

    pnm::coro::Task<pnm::EmptyResult<ITransport::Error>> Bridge::receiveLoop(std::stop_token stop)
    {
        // This buffer lives in the coroutine frame across suspension and is
        // reused only after the previous frame has been dispatched.
        std::array<std::byte, MAX_FRAME_BYTES> buffer{};
        while (!stop.stop_requested()) {
            auto result = co_await m_transport.receive(buffer, stop);
            if (!result) {
                if (result.error() == ITransport::Error::Cancelled && stop.stop_requested())
                    break;
                // Includes BufferTooSmall: return the failure instead of
                // repeatedly retrying the same frame with the same buffer.
                co_return std::unexpected(result.error());
            }
            if (stop.stop_requested())
                break;

            if (*result > buffer.size())
                throw std::length_error("Transport returned a frame larger than the receive buffer");

            dispatchIncomingFrame(std::span<const std::byte>(buffer).first(*result));
        }
        co_return pnm::EmptyResult<ITransport::Error>{};
    }
}
