#include "Bridge.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace p2p
{
    Bridge::Bridge(pnm::msg::Bus& bus, pnm::coro::Context& context, ITransport& transport)
      : m_bus(bus)
      , m_context(context)
      , m_transport(transport)
    {
    }

    Bridge::~Bridge() { stop(); }

    void Bridge::start()
    {
        std::unique_lock lock(m_mutex);
        if (m_started)
            throw std::logic_error("Bridge can only be started once");
        m_started = true;

        try {
            // Construct both coroutine chains before scheduling either one.
            // Moving these ready-made tasks into co_spawn also avoids allocating
            // a new loop task inside its detached entry callback.
            auto sender = runLoop(sendLoop(m_stopSource.get_token()), m_sendRunning);
            auto receiver = runLoop(receiveLoop(m_stopSource.get_token()), m_receiveRunning);

            pnm::coro::co_spawn(m_context, std::move(sender));
            m_sendRunning = true;

            pnm::coro::co_spawn(m_context, std::move(receiver));
            m_receiveRunning = true;
            // Holding the mutex prevents an immediately completing handler from
            // clearing its flag before its scheduling has been recorded.
        } catch (...) {
            m_failure = std::current_exception();
            lock.unlock();
            // If only the first handler was scheduled, it must still finish.
            // Keep driving the context and call stop() before destruction.
            m_stopSource.request_stop();
            throw;
        }
    }

    void Bridge::stop()
    {
        {
            std::scoped_lock lock(m_mutex);
            if (!m_started)
                return;
        }

        m_stopSource.request_stop();

        std::unique_lock lock(m_mutex);
        m_finished.wait(lock, [this] { return !m_sendRunning && !m_receiveRunning; });
        m_outgoingTopics.clear();
        m_incomingTopics.clear();
        m_outgoingWakeup->ready.close();
    }

    std::optional<Bridge::Failure> Bridge::failure() const
    {
        std::scoped_lock lock(m_mutex);
        return m_failure;
    }

    void Bridge::OutgoingWakeup::notify() noexcept
    {
        if (pending.test_and_set())
            return;
        try {
            ready.push(true);
        } catch (...) {
            // Readiness callbacks cannot throw. Closing wakes the sender so it
            // can report a notification failure instead of silently waiting forever.
            ready.close();
        }
    }

    void Bridge::checkTopicRegistration(std::string_view topic) const
    {
        if (m_started)
            throw std::logic_error("Register bridge topics before start");
        if (topic.empty())
            throw std::invalid_argument("Bridge topics require a name");
        if (topic.size() > MAX_FRAME_BYTES - HEADER_BYTES)
            throw std::length_error("Topic name exceeds the bridge frame capacity");
        const auto duplicate = [topic](const auto& registered) { return registered.name == topic; };
        if (std::ranges::any_of(m_outgoingTopics, duplicate) ||
            std::ranges::any_of(m_incomingTopics, duplicate))
            throw std::invalid_argument("Bridge topic must be registered only once");
        if (m_outgoingTopics.size() + m_incomingTopics.size() >= MAX_TOPICS)
            throw std::length_error("Bridge topic capacity reached");
    }

    Bridge::OutgoingFrame Bridge::makeOutgoingFrame(const std::string& topic, std::size_t payloadSize)
    {
        const auto topicSize = pnm::utils::memory::SerializationAdapter<std::string>::bufferSize(topic);
        if (topicSize > MAX_FRAME_BYTES - HEADER_BYTES ||
            payloadSize > MAX_FRAME_BYTES - HEADER_BYTES - topicSize)
            throw std::length_error("Serialized topic exceeds the bridge frame capacity");

        OutgoingFrame frame;
        frame.size = HEADER_BYTES + topicSize + payloadSize;
        frame.bytes[0] = static_cast<std::byte>(FRAME_VERSION);
        frame.bytes[1] = static_cast<std::byte>(topicSize & 0xFFU);
        frame.bytes[2] = static_cast<std::byte>(topicSize >> 8U);
        if (!pnm::utils::memory::serialize(topic, std::span(frame.bytes).subspan(HEADER_BYTES, topicSize)))
            throw std::length_error("Cannot serialize topic name into its frame");
        return frame;
    }

    pnm::coro::Task<std::optional<Bridge::OutgoingFrame>> Bridge::nextOutgoingFrame(std::stop_token stop)
    {
        while (!stop.stop_requested()) {
            // Scan each bounded subscription once. Rotating the starting point
            // prevents one frequently published topic from starving the others.
            for (std::size_t offset = 0; offset < m_outgoingTopics.size(); ++offset) {
                const auto index = (m_nextOutgoingTopic + offset) % m_outgoingTopics.size();
                if (auto frame = m_outgoingTopics[index].take()) {
                    m_nextOutgoingTopic = (index + 1) % m_outgoingTopics.size();
                    co_return frame;
                }
            }

            if (!(co_await m_outgoingWakeup->ready.next(stop))) {
                if (stop.stop_requested())
                    co_return std::nullopt;
                throw std::runtime_error("Bridge outgoing notification failed");
            }
            // Rescan after clearing the signal. A publication racing this clear
            // either remains in its subscription or supplies the next wakeup.
            m_outgoingWakeup->pending.clear();
        }
        co_return std::nullopt;
    }

    void Bridge::dispatchIncomingFrame(std::span<const std::byte> frame)
    {
        if (frame.size() < HEADER_BYTES || frame.size() > MAX_FRAME_BYTES)
            throw std::invalid_argument("Invalid bridge frame size");
        if (std::to_integer<std::uint8_t>(frame[0]) != FRAME_VERSION)
            throw std::invalid_argument("Unsupported bridge frame version");

        const auto topicSize =
          std::to_integer<std::size_t>(frame[1]) | (std::to_integer<std::size_t>(frame[2]) << 8U);
        if (topicSize == 0 || topicSize > frame.size() - HEADER_BYTES)
            throw std::invalid_argument("Invalid bridge topic length");

        std::string name;
        if (!pnm::utils::memory::deserialize(frame.subspan(HEADER_BYTES, topicSize), name))
            throw std::invalid_argument("Invalid bridge topic name");
        const auto topic = std::ranges::find(m_incomingTopics, name, &IncomingTopic::name);
        if (topic == m_incomingTopics.end())
            throw std::invalid_argument("Unknown incoming bridge topic");

        topic->publish(frame.subspan(HEADER_BYTES + topicSize));
    }

    pnm::coro::Task<void> Bridge::runLoop(pnm::coro::Task<pnm::EmptyResult<ITransport::Error>> loop,
                                          bool& running)
    {
        std::optional<Failure> failure;
        try {
            auto result = co_await loop;
            if (!result)
                failure = result.error();
        } catch (...) {
            failure = std::current_exception();
        }

        // Awaiting completion does not itself destroy the owned task. Release
        // its frame before stop() is allowed to return or the bridge can die.
        loop = {};

        {
            std::scoped_lock lock(m_mutex);
            if (failure && !m_failure)
                m_failure = std::move(failure);
        }
        m_stopSource.request_stop();

        // This is our last access to the bridge. Notify while holding the lock
        // so a waking destructor cannot destroy the condition variable early.
        std::scoped_lock lock(m_mutex);
        running = false;
        m_finished.notify_all();
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
