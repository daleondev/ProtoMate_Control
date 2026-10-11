#pragma once

#include "Catalog.hpp"
#include "Frame.hpp"
#include "OutgoingWakeup.hpp"

#include <pneumo/coroutines.hpp>
#include <pneumo/meta.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace p2p
{
    // Owns deferred proxies, correlated calls and their timers. Bridge serializes
    // registration and calls poll()/takeFrame()/dispatch() on its context thread.
    // close() drains calls before the context stops. This helper never joins workers
    // or executes the application's service handlers.
    class ServiceBridge final
    {
        friend class Bridge;
        using OutgoingFrame = util::OutgoingFrame;
        using FrameKind = util::FrameKind;
        using Clock = std::chrono::steady_clock;
        using ServiceError = pnm::msg::ServiceError;
        static constexpr std::size_t MAX_SERVICES{ 16 };
        static constexpr std::size_t MAX_CALLS{ 8 };

        ServiceBridge(pnm::msg::Bus& bus,
                      pnm::coro::Context& context,
                      std::shared_ptr<util::OutgoingWakeup> wakeup,
                      std::stop_token stop);
        ~ServiceBridge() = default;

      public:
        ServiceBridge(const ServiceBridge&) = delete;
        ServiceBridge& operator=(const ServiceBridge&) = delete;
        ServiceBridge(ServiceBridge&&) = delete;
        ServiceBridge& operator=(ServiceBridge&&) = delete;

      private:

        template<typename Catalog, typename Provider>
            requires std::is_enum_v<Provider>
        void registerCatalog(Provider local_provider);
        template<pnm::utils::memory::Serializable Request, pnm::utils::memory::Serializable Response>
            requires std::default_initializable<Request> && std::default_initializable<Response> &&
                     std::move_constructible<Response>
        void registerOutgoing(std::string_view service, Lane lane);
        template<pnm::utils::memory::Serializable Request, pnm::utils::memory::Serializable Response>
            requires std::default_initializable<Request> && std::default_initializable<Response> &&
                     std::move_constructible<Response>
        void registerIncoming(std::string_view service, Lane lane);

        void poll();
        std::optional<OutgoingFrame> takeFrame(Lane lane);
        void dispatch(const util::IncomingFrame& frame, Lane lane);
        void dispatchRequest(const util::IncomingFrame& frame, Lane lane);
        void dispatchCancellation(const util::IncomingFrame& frame, Lane lane);
        void dispatchResponse(const util::IncomingFrame& frame, Lane lane);
        void close(ServiceError error);
        void checkRegistration(std::string_view name) const;
        void queueServiceError(std::uint64_t id, ServiceError error, Lane lane);
        pnm::coro::Context::Timer wakeAfter(Clock::duration delay);
        static OutgoingFrame makeServiceError(std::uint64_t id, ServiceError error);

        // These records are accessed only on the context thread after start().
        // Native readiness hooks and deadline timers only wake the sender.
        struct OutgoingService
        {
            std::string name;
            std::move_only_function<void()> poll;
            pnm::msg::ReadinessRegistration readiness;
        };

        struct OutgoingCall
        {
            std::uint64_t id{};
            Lane lane{ Lane::Normal };
            bool cancel_sent{};
            std::optional<OutgoingFrame> request;
            std::function<bool()> pending;
            std::function<Clock::duration()> remaining;
            std::function<void(std::span<const std::byte>, std::optional<ServiceError>)> finish;
            pnm::msg::ReadinessRegistration readiness;
            pnm::coro::Context::Timer timer;
        };

        struct IncomingCall
        {
            std::uint64_t id{};
            Lane lane{ Lane::Normal };
            std::move_only_function<std::optional<OutgoingFrame>()> take;
            std::move_only_function<void()> cancel;
            pnm::msg::ReadinessRegistration readiness;
            pnm::coro::Context::Timer timer;
        };

        struct IncomingService
        {
            std::string name;
            Lane lane;
            std::function<IncomingCall(std::uint64_t, std::span<const std::byte>, std::chrono::milliseconds)>
              request;
        };

        pnm::msg::Bus& m_bus;
        pnm::coro::Context& m_context;
        std::shared_ptr<util::OutgoingWakeup> m_outgoingWakeup;
        std::stop_token m_stop;
        std::vector<OutgoingService> m_outgoingServices;
        std::vector<IncomingService> m_incomingServices;
        std::vector<OutgoingCall> m_outgoingCalls;
        std::vector<IncomingCall> m_incomingCalls;
        std::deque<std::pair<Lane, OutgoingFrame>> m_serviceReplies;
        std::vector<std::pair<std::uint64_t, Lane>> m_earlyCancellations;
        std::uint64_t m_lastCallId{};
        std::array<std::uint64_t, 2> m_lastIncomingCallId{};
        ServiceError m_serviceShutdownError{ ServiceError::Cancelled };
    };

    template<typename Catalog, typename Provider>
        requires std::is_enum_v<Provider>
    void ServiceBridge::registerCatalog(Provider local_provider)
    {
        static constexpr auto enumerators{ pnm::meta::enumeration::enumerators<Provider>() };
        static_assert(enumerators.size() == 2, "A service provider enum must declare exactly two values");
        constexpr auto first{ enumerators[0] };
        constexpr auto second{ enumerators[1] };
        static_assert(first != second, "Service providers must have distinct values");
        if (local_provider != first && local_provider != second)
            throw std::invalid_argument("Unknown local service provider");

        static constexpr auto members{ std::define_static_array(
          std::meta::members_of(^^Catalog, std::meta::access_context::current())) };

        // Clang synthesizes reserved identifiers for expansion statements.
        // NOLINTBEGIN(bugprone-reserved-identifier,readability-identifier-naming)
        template for (constexpr auto member : members)
        {
            // NOLINTEND(bugprone-reserved-identifier,readability-identifier-naming)
            constexpr auto providers{ std::define_static_array(
              util::annotations_of_type<Provider>(member)) };
            if constexpr (!providers.empty()) {
                static_assert(providers.size() == 1, "A service must have exactly one provider");
                static_assert(std::meta::is_nonstatic_data_member(member),
                              "Annotate nonstatic service fields, not static declarations");
                constexpr auto provider{ std::meta::extract<Provider>(providers[0]) };
                static_assert(provider == first || provider == second, "Unknown annotated service provider");

                if constexpr (std::meta::is_nonstatic_data_member(member)) {
                    using Metadata = pnm::msg::ServiceMetadata<member>;
                    using Request = typename Metadata::Request;
                    using Response = typename Metadata::Response;
                    static constexpr auto lanes =
                      std::define_static_array(util::annotations_of_type<Lane>(member));
                    static_assert(lanes.size() <= 1, "A service may select only one transport lane");
                    constexpr auto lane = [] {
                        if constexpr (lanes.empty())
                            return Lane::Normal;
                        else
                            return std::meta::extract<Lane>(lanes[0]);
                    }();
                    static_assert(lane == Lane::Normal || lane == Lane::Urgent, "Unknown service lane");
                    if (provider == local_provider)
                        registerIncoming<Request, Response>(Metadata::name, lane);
                    else
                        registerOutgoing<Request, Response>(Metadata::name, lane);
                }
            }
        }
    }

    template<pnm::utils::memory::Serializable Request, pnm::utils::memory::Serializable Response>
        requires std::default_initializable<Request> && std::default_initializable<Response> &&
                 std::move_constructible<Response>
    void ServiceBridge::registerOutgoing(std::string_view service, Lane lane)
    {
        checkRegistration(service);
        auto proxy = m_bus.service<Request, Response>(service).serveDeferred(
          [this, name = std::string(service), lane](const Request& value, pnm::msg::Reply<Response> reply) {
            if (m_stop.stop_requested()) {
                reply.fail(m_serviceShutdownError);
                return;
            }
            if (std::ranges::count(m_outgoingCalls, lane, &OutgoingCall::lane) >=
                static_cast<std::ptrdiff_t>(MAX_CALLS)) {
                reply.fail(ServiceError::Busy);
                return;
            }
            // Call IDs never wrap or get reused, including after cancellation.
            if (m_lastCallId == std::numeric_limits<std::uint64_t>::max()) {
                reply.fail(ServiceError::TransportError);
                return;
            }
            auto owned = std::make_shared<pnm::msg::Reply<Response>>(std::move(reply));
            try {
                const auto id = ++m_lastCallId;
                auto frame = util::encode_frame(FrameKind::Request, name, id, value);
                auto timer = wakeAfter(owned->remainingTime());
                auto readiness = pnm::msg::watch(*owned, [signal = m_outgoingWakeup] { signal->notify(); });
                m_outgoingCalls.push_back({
                  .id = id,
                  .lane = lane,
                  .request = std::move(frame),
                  .pending = [owned] { return owned->pending(); },
                  .remaining = [owned] { return owned->remainingTime(); },
                  .finish =
                    [owned](std::span<const std::byte> bytes, std::optional<ServiceError> error) {
                    if (error) {
                        owned->fail(*error);
                        return;
                    }
                    try {
                        Response response{};
                        if (!pnm::utils::memory::deserialize(bytes, response))
                            throw std::invalid_argument("Invalid service response");
                        owned->respond(response);
                    } catch (...) {
                        owned->fail(ServiceError::TransportError);
                    }
                },
                  .readiness = std::move(readiness),
                  .timer = std::move(timer),
                });
            } catch (...) {
                // Oversized/unserializable requests never go to the peer.
                owned->fail(ServiceError::TransportError);
            }
        }, { .max_pending = MAX_CALLS });
        auto readiness = pnm::msg::watch(proxy, [signal = m_outgoingWakeup] { signal->notify(); });
        m_outgoingServices.push_back({
          .name = std::string(service),
          .poll = [proxy = std::move(proxy)]() mutable { proxy.poll(); },
          .readiness = std::move(readiness),
        });
    }

    template<pnm::utils::memory::Serializable Request, pnm::utils::memory::Serializable Response>
        requires std::default_initializable<Request> && std::default_initializable<Response> &&
                 std::move_constructible<Response>
    void ServiceBridge::registerIncoming(std::string_view service, Lane lane)
    {
        checkRegistration(service);
        auto request =
          [this, local = m_bus.service<Request, Response>(service), lane](
            std::uint64_t id, std::span<const std::byte> bytes, std::chrono::milliseconds timeout) {
            Request value{};
            if (!pnm::utils::memory::deserialize(bytes, value))
                throw std::invalid_argument("Invalid service request");
            // request() enqueues for the application's provider; it never executes
            // its handler here. Only inspect get() after ready() confirms completion.
            auto call = std::make_shared<pnm::msg::PendingCall<Response>>(local.request(value, timeout));
            auto readiness = pnm::msg::watch(*call, [signal = m_outgoingWakeup] { signal->notify(); });
            auto timer = wakeAfter(timeout);
            return IncomingCall{
                .id = id,
                .lane = lane,
                .take = [call, id]() -> std::optional<OutgoingFrame> {
                if (!call->ready())
                    return std::nullopt;
                auto result = call->get();
                if (!result)
                    return makeServiceError(id, result.error());
                try {
                    return util::encode_frame(FrameKind::Response, {}, id, *result);
                } catch (...) {
                    return makeServiceError(id, ServiceError::TransportError);
                }
            },
                .cancel = [call] { call->cancel(); },
                .readiness = std::move(readiness),
                .timer = std::move(timer),
            };
        };
        m_incomingServices.push_back(
          { .name = std::string(service), .lane = lane, .request = std::move(request) });
    }
}
