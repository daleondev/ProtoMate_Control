#pragma once

#include "Catalog.hpp"
#include "Frame.hpp"
#include "OutgoingWakeup.hpp"

#include <pneumo/coroutines.hpp>

#include <chrono>
#include <concepts>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace icc
{
    // Bridge owns routing; application workers own execution. After registration,
    // only the icc context accesses these records. Readiness callbacks only notify.
    class ActionBridge final
    {
        friend class Bridge;
        using Frame = util::OutgoingFrame;
        using Kind = util::FrameKind;
        using Error = pnm::msg::ActionError;
        using Status = pnm::msg::ActionStatus;
        using Clock = std::chrono::steady_clock;
        static constexpr std::size_t MAX_ACTIONS{ 16 };
        static constexpr std::size_t MAX_GOALS{ 8 };

        ActionBridge(pnm::msg::Bus& bus,
                     pnm::coro::Context& context,
                     std::shared_ptr<util::OutgoingWakeup> wakeup,
                     std::stop_token stop);
        ~ActionBridge();

      public:
        ActionBridge(const ActionBridge&) = delete;
        ActionBridge& operator=(const ActionBridge&) = delete;
        ActionBridge(ActionBridge&&) = delete;
        ActionBridge& operator=(ActionBridge&&) = delete;

      private:
        template<typename Catalog, typename Provider>
            requires std::is_enum_v<Provider>
        void registerCatalog(Provider local_provider);
        template<pnm::utils::memory::Serializable G,
                 pnm::utils::memory::Serializable F,
                 pnm::utils::memory::Serializable R>
        void registerOutgoing(std::string_view name);
        template<pnm::utils::memory::Serializable G,
                 pnm::utils::memory::Serializable F,
                 pnm::utils::memory::Serializable R>
        void registerIncoming(std::string_view name);

        template<typename F, typename R>
        static void deliverEvent(pnm::msg::ActionExecution<F, R>& execution,
                                 const util::IncomingFrame& incoming);

        void poll();
        std::optional<Frame> takeFrame(Lane lane);
        void dispatch(const util::IncomingFrame& frame, Lane lane);
        void dispatchGoal(const util::IncomingFrame& frame);
        void dispatchControl(const util::IncomingFrame& frame);
        static std::pair<std::optional<Status>, std::optional<Error>> decodeEvent(
          const util::IncomingFrame& frame);
        void dispatchEvent(const util::IncomingFrame& frame);
        std::optional<Frame> takeNormalFrame();
        void requestStop();
        [[nodiscard]] bool drained() const;
        // Failure requests local cleanup but cannot confirm a physical stop.
        void close(Error error);
        void checkRegistration(std::string_view name) const;
        pnm::coro::Context::Timer wakeAfter(Clock::duration delay);
        void reject(std::uint64_t id, Error error);
        static Frame makeError(std::uint64_t id, Error error);
        static Error decodeError(std::span<const std::byte> payload);
        static std::byte encodeStatus(Status status);
        static Status decodeStatus(std::byte status);
        template<pnm::utils::memory::Serializable R>
        static Frame makeResult(std::uint64_t id, Status status, const R& value);

        struct OutgoingRoute
        {
            std::string name;
            std::move_only_function<void()> poll;
            pnm::msg::ReadinessRegistration readiness;
        };
        struct OutgoingGoal
        {
            std::uint64_t id{};
            std::optional<Frame> request;
            std::function<bool()> pending, cancelled;
            std::function<void()> cancel, cancel_before_submission;
            std::function<Clock::duration()> remaining;
            std::function<bool()> accept;
            std::function<void(const util::IncomingFrame&)> deliver;
            std::function<void(Error)> fail;
            pnm::msg::ReadinessRegistration readiness;
            pnm::coro::Context::Timer timer;
            bool accepted{}, cancel_sent{};
        };
        // Callbacks retain only their own event record, never vector references.
        struct Events
        {
            bool accepted{}, acceptance_pending{}, encoding_failed{}, terminal_sent{};
            std::optional<Frame> feedback, terminal;
        };
        struct IncomingGoal
        {
            std::uint64_t id{};
            std::shared_ptr<Events> events;
            std::move_only_function<void()> poll, cancel;
            pnm::msg::ReadinessRegistration readiness;
            pnm::coro::Context::Timer timer;
        };
        struct IncomingRoute
        {
            std::string name;
            std::function<IncomingGoal(std::uint64_t, std::span<const std::byte>, std::chrono::milliseconds)>
              submit;
            std::function<Frame(std::uint64_t)> cancelled;
        };
        struct Rejection
        {
            std::uint64_t id{};
            Frame terminal;
            bool sent{};
        };

        // Return true once the outgoing record can be released.
        static bool deliverToGoal(OutgoingGoal& goal,
                                  const util::IncomingFrame& frame,
                                  std::optional<Status> status,
                                  std::optional<Error> error);

        pnm::msg::Bus& m_bus;
        pnm::coro::Context& m_context;
        std::shared_ptr<util::OutgoingWakeup> m_wakeup;
        std::stop_token m_stop;
        std::vector<OutgoingRoute> m_outgoingRoutes;
        std::vector<IncomingRoute> m_incomingRoutes;
        std::vector<OutgoingGoal> m_outgoingGoals;
        std::vector<IncomingGoal> m_incomingGoals;
        std::deque<Rejection> m_rejections;
        std::deque<std::uint64_t> m_acknowledgements;
        // Urgent cancellation can overtake a goal in the normal lane.
        std::vector<std::uint64_t> m_earlyCancellations;
        std::uint64_t m_lastOutgoing{}, m_lastIncoming{};
        std::size_t m_nextIncoming{};
        bool m_stopping{}, m_closed{};
        Error m_closeError{ Error::Unavailable };
    };

    template<typename Catalog, typename Provider>
        requires std::is_enum_v<Provider>
    void ActionBridge::registerCatalog(Provider local_provider)
    {
        static constexpr auto owners = pnm::meta::enumeration::enumerators<Provider>();
        static_assert(owners.size() == 2, "An action provider enum must declare exactly two values");
        constexpr auto first = owners[0];
        constexpr auto second = owners[1];
        static_assert(first != second, "Action providers must have distinct values");
        if (local_provider != first && local_provider != second) {
            throw std::invalid_argument("Unknown local action provider");
        }
        // Clang synthesizes reserved identifiers for expansion statements.
        // NOLINTBEGIN(bugprone-reserved-identifier,readability-identifier-naming)
        template for (constexpr auto member : std::define_static_array(
                        std::meta::members_of(^^Catalog, std::meta::access_context::current())))
        {
            // NOLINTEND(bugprone-reserved-identifier,readability-identifier-naming)
            constexpr auto providers = std::define_static_array(util::annotations_of_type<Provider>(member));
            if constexpr (!providers.empty()) {
                static_assert(providers.size() == 1, "An action must have exactly one provider");
                static_assert(std::meta::is_nonstatic_data_member(member),
                              "Annotate nonstatic action fields");
                constexpr auto provider = std::meta::extract<Provider>(providers[0]);
                static_assert(provider == first || provider == second, "Unknown annotated action provider");
                if constexpr (std::meta::is_nonstatic_data_member(member)) {
                    using Metadata = pnm::msg::ActionMetadata<member>;
                    if (provider == local_provider) {
                        registerIncoming<typename Metadata::Goal,
                                         typename Metadata::Feedback,
                                         typename Metadata::Result>(Metadata::name);
                    }
                    else {
                        registerOutgoing<typename Metadata::Goal,
                                         typename Metadata::Feedback,
                                         typename Metadata::Result>(Metadata::name);
                    }
                }
            }
        }
    }

    template<pnm::utils::memory::Serializable G,
             pnm::utils::memory::Serializable F,
             pnm::utils::memory::Serializable R>
    void ActionBridge::registerOutgoing(std::string_view name)
    {
        checkRegistration(name);
        auto proxy = m_bus.action<G, F, R>(name).serveDeferred(
          [this, name = std::string(name)](const G& value, pnm::msg::ActionExecution<F, R> execution) {
            if (m_closed || m_stopping || m_stop.stop_requested()) {
                execution.fail(m_closeError);
                return;
            }
            if (m_outgoingGoals.size() >= MAX_GOALS) {
                execution.fail(Error::Busy);
                return;
            }
            if (m_lastOutgoing == std::numeric_limits<std::uint64_t>::max()) {
                execution.fail(Error::TransportError);
                return;
            }
            auto owned = std::make_shared<pnm::msg::ActionExecution<F, R>>(std::move(execution));
            try {
                const auto id = ++m_lastOutgoing;
                auto frame = util::encode_frame(Kind::ActionGoal, name, id, value);
                auto readiness = pnm::msg::watch(*owned, [signal = m_wakeup] { signal->notifyControl(); });
                auto timer = wakeAfter(owned->remainingAcceptanceTime());
                m_outgoingGoals.push_back({
                  .id = id,
                  .request = std::move(frame),
                  .pending = [owned] { return owned->pending(); },
                  .cancelled = [owned] { return owned->cancelRequested(); },
                  .cancel = [owned] { owned->requestCancel(); },
                  .cancel_before_submission = [owned] { owned->cancelled(R{}); },
                  .remaining = [owned] { return owned->remainingAcceptanceTime(); },
                  .accept = [owned] { return owned->accept(); },
                  .deliver = [owned](const util::IncomingFrame& incoming) { deliverEvent(*owned, incoming); },
                  .fail = [owned](Error error) { owned->fail(error); },
                  .readiness = std::move(readiness),
                  .timer = std::move(timer),
                });
            } catch (...) {
                owned->fail(Error::TransportError);
            }
        }, { .max_goals = MAX_GOALS });
        auto readiness = pnm::msg::watch(proxy, [signal = m_wakeup] { signal->notifyControl(); });
        m_outgoingRoutes.push_back({ .name = std::string(name), .poll = [proxy = std::move(proxy)]() mutable {
            proxy.poll();
        }, .readiness = std::move(readiness) });
    }

    template<typename F, typename R>
    void ActionBridge::deliverEvent(pnm::msg::ActionExecution<F, R>& execution,
                                    const util::IncomingFrame& incoming)
    {
        try {
            if (incoming.kind == Kind::ActionError) {
                execution.fail(decodeError(incoming.payload));
            }
            else if (incoming.kind == Kind::ActionFeedback) {
                F feedback{};
                if (!pnm::utils::memory::deserialize(incoming.payload, feedback)) {
                    throw std::invalid_argument("Invalid action feedback");
                }
                execution.feedback(feedback);
            }
            else {
                const auto status = decodeStatus(incoming.payload.front());
                R result{};
                if (!pnm::utils::memory::deserialize(incoming.payload.subspan(1), result)) {
                    throw std::invalid_argument("Invalid action result");
                }
                switch (status) {
                    case Status::Succeeded:
                        execution.succeed(result);
                        break;
                    case Status::Aborted:
                        execution.abort(result);
                        break;
                    case Status::Cancelled:
                        execution.cancelled(result);
                        break;
                    case Status::Rejected:
                        execution.reject(result);
                        break;
                }
            }
        } catch (...) {
            execution.fail(Error::TransportError);
        }
    }

    template<pnm::utils::memory::Serializable G,
             pnm::utils::memory::Serializable F,
             pnm::utils::memory::Serializable R>
    void ActionBridge::registerIncoming(std::string_view name)
    {
        checkRegistration(name);
        auto submit = [this, action = m_bus.action<G, F, R>(name)](std::uint64_t id,
                                                                   std::span<const std::byte> bytes,
                                                                   std::chrono::milliseconds timeout) {
            G value{};
            if (!pnm::utils::memory::deserialize(bytes, value)) {
                throw std::invalid_argument("Invalid action goal");
            }
            auto events = std::make_shared<Events>();
            auto handle = std::make_shared<pnm::msg::PendingGoal<F, R>>(
              action.sendGoal(value, { .accept_timeout = timeout }, { .on_accepted = [events] {
                events->accepted = true;
                events->acceptance_pending = true;
            }, .on_feedback = [events, id](const F& feedback) {
                try {
                    events->feedback = util::encode_frame(Kind::ActionFeedback, {}, id, feedback);
                } catch (...) {
                    events->encoding_failed = true;
                    events->feedback.reset();
                }
            }, .on_result = [events, id](pnm::msg::ActionResult<R> result) {
                try {
                    if (events->encoding_failed) {
                        events->terminal = makeError(id, Error::TransportError);
                    }
                    else if (!result) {
                        events->terminal = makeError(id, result.error());
                    }
                    else {
                        events->terminal = makeResult(id, result->status, result->value);
                    }
                } catch (...) {
                    events->terminal = makeError(id, Error::TransportError);
                }
            } }));
            auto readiness = pnm::msg::watch(*handle, [signal = m_wakeup] { signal->notifyControl(); });
            auto timer = wakeAfter(timeout);
            return IncomingGoal{
                .id = id,
                .events = events,
                .poll =
                  [handle, events] {
                handle->poll();
                if (events->encoding_failed && !events->terminal) {
                    handle->requestCancel();
                }
            },
                .cancel = [handle] { handle->requestCancel(); },
                .readiness = std::move(readiness),
                .timer = std::move(timer),
            };
        };
        m_incomingRoutes.push_back({ std::string(name), std::move(submit), [](std::uint64_t id) {
            return makeResult(id, Status::Cancelled, R{});
        } });
    }

    template<pnm::utils::memory::Serializable R>
    ActionBridge::Frame ActionBridge::makeResult(std::uint64_t id, Status status, const R& value)
    {
        std::size_t size{ sizeof(R) };
        if constexpr (pnm::utils::memory::HasSerializationAdapter<R>) {
            size = pnm::utils::memory::SerializationAdapter<R>::bufferSize(value);
        }
        if (size > util::MAX_FRAME_BYTES - util::HEADER_BYTES - 1) {
            throw std::length_error("Action result exceeds frame capacity");
        }
        auto frame = util::make_frame(Kind::ActionResult, {}, id, size + 1);
        frame.bytes[util::HEADER_BYTES] = encodeStatus(status);
        if (!pnm::utils::memory::serialize(value,
                                           std::span(frame.bytes).subspan(util::HEADER_BYTES + 1, size))) {
            throw std::invalid_argument("Cannot serialize action result");
        }
        return frame;
    }

}
