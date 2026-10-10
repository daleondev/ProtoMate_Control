#include "InProcessTransport.hpp"
#include <deque>
#include <mutex>
#include <stdexcept>

namespace p2p
{
    struct InProcessTransport::Shared
    {
        explicit Shared(Limits value)
          : limits(value)
        {
        }
        Limits limits;
        mutable std::mutex mutex;
        bool connected{ true };
        std::array<std::array<std::deque<Bytes>, 2>, 2> inbox;
        std::array<std::function<void()>, 2> wake;
    };
    InProcessTransport::InProcessTransport(std::shared_ptr<Shared> shared, unsigned side)
      : m_shared(std::move(shared))
      , m_side(side)
    {
    }
    InProcessTransport::~InProcessTransport() { setWakeup({}); }
    auto InProcessTransport::pair() -> std::array<std::shared_ptr<InProcessTransport>, 2>
    {
        return pair(Limits{});
    }
    auto InProcessTransport::pair(Limits limits) -> std::array<std::shared_ptr<InProcessTransport>, 2>
    {
        if (!limits.normal_frames || !limits.urgent_frames || limits.frame_bytes < 64 ||
            limits.urgent_bytes < 64)
            throw std::invalid_argument("invalid p2p transport limits");
        auto shared = std::make_shared<Shared>(limits);
        return { std::shared_ptr<InProcessTransport>(new InProcessTransport(shared, 0)),
                 std::shared_ptr<InProcessTransport>(new InProcessTransport(shared, 1)) };
    }
    SendResult InProcessTransport::trySend(std::span<const std::byte> frame, Lane lane)
    {
        std::function<void()> wake;
        {
            std::scoped_lock lock(m_shared->mutex);
            if (!m_shared->connected)
                return SendResult::Disconnected;
            const bool urgent = lane == Lane::Urgent;
            if (frame.size() > (urgent ? m_shared->limits.urgent_bytes : m_shared->limits.frame_bytes))
                throw std::length_error("p2p frame exceeds transport limit");
            auto& queue = m_shared->inbox[1U - m_side][urgent ? 1 : 0];
            if (queue.size() >= (urgent ? m_shared->limits.urgent_frames : m_shared->limits.normal_frames))
                return SendResult::Full;
            queue.emplace_back(frame.begin(), frame.end());
            wake = m_shared->wake[1U - m_side];
        }
        if (wake)
            wake();
        return SendResult::Sent;
    }
    bool InProcessTransport::tryReceive(Bytes& frame)
    {
        std::function<void()> wake;
        {
            std::scoped_lock lock(m_shared->mutex);
            if (!m_shared->connected)
                return false;
            auto& inbox = m_shared->inbox[m_side];
            auto& queue = inbox[inbox[1].empty() ? 0 : 1];
            if (queue.empty())
                return false;
            frame = std::move(queue.front());
            queue.pop_front();
            wake = m_shared->wake[1U - m_side]; // Sender can now retry retained data.
        }
        if (wake)
            wake();
        return true;
    }
    bool InProcessTransport::connected() const
    {
        std::scoped_lock lock(m_shared->mutex);
        return m_shared->connected;
    }
    Capacity InProcessTransport::capacity(Lane lane) const
    {
        std::scoped_lock lock(m_shared->mutex);
        const bool urgent = lane == Lane::Urgent;
        const auto count = urgent ? m_shared->limits.urgent_frames : m_shared->limits.normal_frames;
        const auto queued = m_shared->inbox[1U - m_side][urgent ? 1 : 0].size();
        return { m_shared->connected ? count - queued : 0,
                 urgent ? m_shared->limits.urgent_bytes : m_shared->limits.frame_bytes };
    }
    void InProcessTransport::setWakeup(std::function<void()> callback)
    {
        auto initial = callback;
        {
            std::scoped_lock lock(m_shared->mutex);
            m_shared->wake[m_side] = std::move(callback);
        }
        if (initial)
            initial();
    }
    void InProcessTransport::setConnected(bool connected)
    {
        std::array<std::function<void()>, 2> wake;
        {
            std::scoped_lock lock(m_shared->mutex);
            if (m_shared->connected == connected)
                return;
            m_shared->connected = connected;
            for (auto& inbox : m_shared->inbox)
                for (auto& queue : inbox)
                    queue.clear();
            wake = m_shared->wake;
        }
        for (auto& callback : wake)
            if (callback)
                callback();
    }
}
