#pragma once

#include "Frame.hpp"
#include "OutgoingWakeup.hpp"

#include <pneumo/coroutines.hpp>
#include <pneumo/meta.hpp>

#include <concepts>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace p2p
{
    // Bridge owns this helper. Registration runs under Bridge's lifecycle lock;
    // takeFrame()/dispatch() run only on its context thread. No transport access.
    class TopicBridge final
    {
        friend class Bridge;
        using OutgoingFrame = util::OutgoingFrame;
        using FrameKind = util::FrameKind;
        static constexpr std::size_t MAX_TOPICS{ 16 };

        TopicBridge(pnm::msg::Bus& bus, std::shared_ptr<util::OutgoingWakeup> wakeup);
        TopicBridge(const TopicBridge&) = delete;
        TopicBridge& operator=(const TopicBridge&) = delete;
        TopicBridge(TopicBridge&&) = delete;
        TopicBridge& operator=(TopicBridge&&) = delete;

        template<typename Catalog, typename Publisher>
            requires std::is_enum_v<Publisher>
        void registerCatalog(Publisher localPublisher);
        template<pnm::utils::memory::Serializable T>
            requires std::default_initializable<T>
        void registerOutgoing(std::string_view topic);
        template<pnm::utils::memory::Serializable T>
            requires std::default_initializable<T>
        void registerIncoming(std::string_view topic);

        void checkRegistration(std::string_view topic) const;
        std::optional<OutgoingFrame> takeFrame();
        void dispatch(const util::IncomingFrame& frame);
        void close();

        struct OutgoingTopic
        {
            std::string name;
            std::move_only_function<std::optional<OutgoingFrame>()> take;
            pnm::msg::ReadinessRegistration readiness;
        };
        struct IncomingTopic
        {
            std::string name;
            std::function<void(std::span<const std::byte>)> publish;
        };

        pnm::msg::Bus& m_bus;
        std::shared_ptr<util::OutgoingWakeup> m_outgoingWakeup;
        std::vector<OutgoingTopic> m_outgoingTopics;
        std::vector<IncomingTopic> m_incomingTopics;
        std::size_t m_nextOutgoingTopic{};
    };

    template<typename Catalog, typename Publisher>
        requires std::is_enum_v<Publisher>
    void TopicBridge::registerCatalog(Publisher localPublisher)
    {
        static constexpr auto enumerators{ pnm::meta::enumeration::enumerators<Publisher>() };
        static_assert(enumerators.size() == 2, "A topic publisher enum must declare exactly two values");

        constexpr auto first{ enumerators[0] };
        constexpr auto second{ enumerators[1] };
        static_assert(std::to_underlying(first) != std::to_underlying(second),
                      "Topic publishers must have distinct values");

        if (localPublisher != first && localPublisher != second) {
            throw std::invalid_argument("Unknown local topic publisher");
        }

        static constexpr auto members{ std::define_static_array(
          std::meta::members_of(^^Catalog, std::meta::access_context::current())) };

        template for (constexpr auto member : members)
        {
            constexpr auto publishers{ std::define_static_array(
              std::meta::annotations_of_with_type(member, ^^Publisher)) };
            if constexpr (!publishers.empty()) {
                static_assert(publishers.size() == 1, "A topic must have exactly one publisher");
                static_assert(std::meta::is_nonstatic_data_member(member),
                              "Annotate nonstatic topic fields, not static declarations");
                constexpr auto publisher{ std::meta::extract<Publisher>(publishers[0]) };
                static_assert(publisher == first || publisher == second, "Unknown annotated topic publisher");

                if constexpr (std::meta::is_nonstatic_data_member(member)) {
                    using Metadata = pnm::msg::TopicMetadata<member>;
                    using Message = typename Metadata::Message;
                    if (publisher == localPublisher)
                        registerOutgoing<Message>(Metadata::name);
                    else
                        registerIncoming<Message>(Metadata::name);
                }
            }
        }
    }

    template<pnm::utils::memory::Serializable T>
        requires std::default_initializable<T>
    void TopicBridge::registerOutgoing(std::string_view topic)
    {
        checkRegistration(topic);

        // The typed callback runs only when takeFrame() polls it on the
        // context thread. Its result is moved straight into the send coroutine.
        auto pending = std::make_shared<std::optional<OutgoingFrame>>();
        auto subscription =
          m_bus.topic<T>(topic).subscribe([pending, name = std::string(topic)](const T& value) {
            *pending = util::encodeFrame(FrameKind::Topic, name, 0, value);
        }, pnm::msg::SubscriptionOptions::latestOnly());

        auto readiness = pnm::msg::watch(subscription, [signal = m_outgoingWakeup] { signal->notify(); });
        auto take = [subscription = std::move(subscription), pending]() mutable {
            subscription.poll(); // Nonblocking; latest-only means at most one callback.
            return std::exchange(*pending, std::nullopt);
        };
        m_outgoingTopics.push_back({
          .name = std::string(topic),
          .take = std::move(take),
          .readiness = std::move(readiness),
        });
    }

    template<pnm::utils::memory::Serializable T>
        requires std::default_initializable<T>
    void TopicBridge::registerIncoming(std::string_view topic)
    {
        checkRegistration(topic);

        auto publish = [localTopic = m_bus.topic<T>(topic)](std::span<const std::byte> payload) {
            T value{};
            if (!pnm::utils::memory::deserialize(payload, value))
                throw std::invalid_argument("Invalid incoming topic payload");
            localTopic.publish(value);
        };
        m_incomingTopics.push_back({
          .name = std::string(topic),
          .publish = std::move(publish),
        });
    }
}
