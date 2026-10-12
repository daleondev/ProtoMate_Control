#pragma once

#include "Catalog.hpp"
#include "Frame.hpp"
#include "OutgoingWakeup.hpp"

#include <pneumo/coroutines.hpp>
#include <pneumo/meta.hpp>

#include <concepts>
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace icc
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
        ~TopicBridge() = default;

      public:
        TopicBridge(const TopicBridge&) = delete;
        TopicBridge& operator=(const TopicBridge&) = delete;
        TopicBridge(TopicBridge&&) = delete;
        TopicBridge& operator=(TopicBridge&&) = delete;

      private:
        template<typename Catalog, typename Publisher>
            requires std::is_enum_v<Publisher>
        void registerCatalog(Publisher local_publisher);
        template<pnm::utils::memory::Serializable T>
            requires std::default_initializable<T>
        void registerOutgoing(std::string_view topic, Lane lane, pnm::msg::SubscriptionOptions options);
        template<pnm::utils::memory::Serializable T>
            requires std::default_initializable<T>
        void registerIncoming(std::string_view topic, Lane lane);

        void checkRegistration(std::string_view topic) const;
        std::optional<OutgoingFrame> takeFrame(Lane lane);
        void dispatch(const util::IncomingFrame& frame, Lane lane);
        [[nodiscard]] std::optional<pnm::msg::SubscriptionStatistics> statistics(std::string_view topic) const;
        void close();

        struct OutgoingTopic
        {
            std::string name;
            Lane lane;
            std::move_only_function<std::optional<OutgoingFrame>()> take;
            std::function<pnm::msg::SubscriptionStatistics()> statistics;
            pnm::msg::ReadinessRegistration readiness;
        };
        struct IncomingTopic
        {
            std::string name;
            Lane lane;
            std::function<void(std::span<const std::byte>)> publish;
        };

        pnm::msg::Bus& m_bus;
        std::shared_ptr<util::OutgoingWakeup> m_outgoingWakeup;
        std::vector<OutgoingTopic> m_outgoingTopics;
        std::vector<IncomingTopic> m_incomingTopics;
        std::array<std::size_t, LANE_COUNT> m_nextOutgoingTopic{};
        // Protect route lifetime during public statistics queries and teardown.
        mutable std::mutex m_mutex;
    };

    template<typename Catalog, typename Publisher>
        requires std::is_enum_v<Publisher>
    void TopicBridge::registerCatalog(Publisher local_publisher)
    {
        static constexpr auto enumerators{ pnm::meta::enumeration::enumerators<Publisher>() };
        static_assert(enumerators.size() == 2, "A topic publisher enum must declare exactly two values");

        constexpr auto first{ enumerators[0] };
        constexpr auto second{ enumerators[1] };
        static_assert(std::to_underlying(first) != std::to_underlying(second),
                      "Topic publishers must have distinct values");

        if (local_publisher != first && local_publisher != second) {
            throw std::invalid_argument("Unknown local topic publisher");
        }

        static constexpr auto members{ std::define_static_array(
          std::meta::members_of(^^Catalog, std::meta::access_context::current())) };

        // Clang synthesizes reserved identifiers for expansion statements.
        // NOLINTBEGIN(bugprone-reserved-identifier,readability-identifier-naming)
        template for (constexpr auto member : members)
        {
            // NOLINTEND(bugprone-reserved-identifier,readability-identifier-naming)
            constexpr auto publishers{ std::define_static_array(
              util::annotations_of_type<Publisher>(member)) };
            if constexpr (!publishers.empty()) {
                static_assert(publishers.size() == 1, "A topic must have exactly one publisher");
                static_assert(std::meta::is_nonstatic_data_member(member),
                              "Annotate nonstatic topic fields, not static declarations");
                constexpr auto publisher{ std::meta::extract<Publisher>(publishers[0]) };
                static_assert(publisher == first || publisher == second, "Unknown annotated topic publisher");

                if constexpr (std::meta::is_nonstatic_data_member(member)) {
                    using Metadata = pnm::msg::TopicMetadata<member>;
                    using Message = typename Metadata::Message;
                    static constexpr auto lanes{ std::define_static_array(util::annotations_of_type<Lane>(member)) };
                    static_assert(lanes.size() <= 1, "A topic may select only one transport lane");
                    constexpr auto lane = [] {
                        if constexpr (lanes.empty()) { return Lane::Normal; }
                        else { return std::meta::extract<Lane>(lanes[0]); }
                    }();
                    static_assert(lane == Lane::Normal || lane == Lane::Background,
                                  "Topics use Normal or Background; urgent capacity is reserved for control");
                    static constexpr auto configurations{ std::define_static_array(
                      util::annotations_of_type<pnm::msg::SubscriptionOptions>(member)) };
                    static_assert(configurations.size() <= 1, "A topic may select only one queue configuration");
                    constexpr auto options = [] {
                        if constexpr (configurations.empty()) { return pnm::msg::SubscriptionOptions::latestOnly(); }
                        else { return std::meta::extract<pnm::msg::SubscriptionOptions>(configurations[0]); }
                    }();
                    static_assert(options.capacity >= 1 && options.capacity <= 64,
                                  "Bridge topic queues must hold 1 to 64 records");
                    static_assert(options.overflow == pnm::msg::OverflowPolicy::DropOldest ||
                                  options.overflow == pnm::msg::OverflowPolicy::DropNewest,
                                  "Unknown topic overflow policy");
                    if (publisher == local_publisher) {
                        registerOutgoing<Message>(Metadata::name, lane, options);
                    }
                    else {
                        registerIncoming<Message>(Metadata::name, lane);
                    }
                }
            }
        }
    }

    template<pnm::utils::memory::Serializable T>
        requires std::default_initializable<T>
    void TopicBridge::registerOutgoing(std::string_view topic, Lane lane, pnm::msg::SubscriptionOptions options)
    {
        const std::scoped_lock lock(m_mutex);
        checkRegistration(topic);

        // The typed callback runs only when takeFrame() polls it on the
        // context thread. Its result is moved straight into the send coroutine.
        auto pending = std::make_shared<std::optional<OutgoingFrame>>();
        auto oversized = std::make_shared<std::atomic<std::uint64_t>>(0);
        auto subscription = std::make_shared<pnm::msg::Subscription<T>>(
          m_bus.topic<T>(topic).subscribe([pending, oversized, lane, name = std::string(topic)](const T& value) {
            try {
                auto frame = util::encode_frame(FrameKind::Topic, name, 0, value);
                if (lane == Lane::Background && frame.size > MAX_BACKGROUND_FRAME_BYTES) {
                    oversized->fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                *pending = std::move(frame);
            } catch (const std::length_error&) {
                if (lane != Lane::Background) { throw; }
                oversized->fetch_add(1, std::memory_order_relaxed);
            }
        }, options));

        auto readiness = pnm::msg::watch(*subscription, [signal = m_outgoingWakeup, lane] {
            signal->signal(lane).notify();
        });
        auto take = [subscription, pending, signal = m_outgoingWakeup, lane]() mutable {
            // One record per turn: preserve FIFO without monopolizing the context.
            subscription->pollSome(1);
            if (subscription->statistics().queued != 0) { signal->signal(lane).notify(); }
            return std::exchange(*pending, std::nullopt);
        };
        m_outgoingTopics.push_back({
          .name = std::string(topic),
          .lane = lane,
          .take = std::move(take),
          .statistics = [subscription, oversized] {
              auto result = subscription->statistics();
              result.dropped += oversized->load(std::memory_order_relaxed);
              return result;
          },
          .readiness = std::move(readiness),
        });
    }

    template<pnm::utils::memory::Serializable T>
        requires std::default_initializable<T>
    void TopicBridge::registerIncoming(std::string_view topic, Lane lane)
    {
        const std::scoped_lock lock(m_mutex);
        checkRegistration(topic);

        auto publish = [local_topic = m_bus.topic<T>(topic)](std::span<const std::byte> payload) {
            T value{};
            if (!pnm::utils::memory::deserialize(payload, value)) {
                throw std::invalid_argument("Invalid incoming topic payload");
            }
            local_topic.publish(value);
        };
        m_incomingTopics.push_back({
          .name = std::string(topic),
          .lane = lane,
          .publish = std::move(publish),
        });
    }
}
