#include "TopicBridge.hpp"

#include <algorithm>

namespace p2p
{
    TopicBridge::TopicBridge(pnm::msg::Bus& bus, std::shared_ptr<util::OutgoingWakeup> wakeup)
      : m_bus(bus)
      , m_outgoingWakeup(std::move(wakeup))
    {
    }

    void TopicBridge::checkRegistration(std::string_view topic) const
    {
        if (topic.empty())
            throw std::invalid_argument("Bridge topics require a name");
        if (topic.size() > util::MAX_FRAME_BYTES - util::HEADER_BYTES)
            throw std::length_error("Topic name exceeds the bridge frame capacity");
        const auto duplicate = [topic](const auto& registered) { return registered.name == topic; };
        if (std::ranges::any_of(m_outgoingTopics, duplicate) ||
            std::ranges::any_of(m_incomingTopics, duplicate))
            throw std::invalid_argument("Bridge topic must be registered only once");
        if (m_outgoingTopics.size() + m_incomingTopics.size() >= MAX_TOPICS)
            throw std::length_error("Bridge topic capacity reached");
    }

    std::optional<util::OutgoingFrame> TopicBridge::takeFrame()
    {
        // Scan each bounded subscription once. Rotating the starting point
        // prevents one frequently published topic from starving the others.
        for (std::size_t offset = 0; offset < m_outgoingTopics.size(); ++offset) {
            const auto index = (m_nextOutgoingTopic + offset) % m_outgoingTopics.size();
            if (auto frame = m_outgoingTopics[index].take()) {
                m_nextOutgoingTopic = (index + 1) % m_outgoingTopics.size();
                return frame;
            }
        }
        return std::nullopt;
    }

    void TopicBridge::dispatch(const util::IncomingFrame& frame)
    {
        if (frame.name.empty() || frame.id != 0 || frame.budget != 0)
            throw std::invalid_argument("Invalid topic header");
        const auto topic = std::ranges::find(m_incomingTopics, frame.name, &IncomingTopic::name);
        if (topic == m_incomingTopics.end())
            throw std::invalid_argument("Unknown incoming bridge topic");
        topic->publish(frame.payload);
    }

    void TopicBridge::close()
    {
        m_outgoingTopics.clear();
        m_incomingTopics.clear();
        m_nextOutgoingTopic = 0;
    }
}
