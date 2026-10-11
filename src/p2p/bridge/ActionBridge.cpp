#include "ActionBridge.hpp"

#include <algorithm>

namespace p2p
{
    namespace
    {
        enum class WireError : std::uint8_t
        {
            Unavailable = 1,
            Busy = 2,
            Rejected = 3,
            Timeout = 4,
            HandlerFailed = 5,
            TransportError = 6,
        };
    }
    ActionBridge::ActionBridge(pnm::msg::Bus& bus,
                               pnm::coro::Context& context,
                               std::shared_ptr<util::OutgoingWakeup> wakeup,
                               std::stop_token stop)
      : m_bus(bus)
      , m_context(context)
      , m_wakeup(std::move(wakeup))
      , m_stop(std::move(stop))
    {
    }

    ActionBridge::~ActionBridge() { close(Error::Unavailable); }

    ActionBridge::Frame ActionBridge::makeError(std::uint64_t id, Error error)
    {
        auto frame = util::make_frame(Kind::ActionError, {}, id, 1);
        switch (error) {
            case Error::Unavailable:
                frame.bytes[util::HEADER_BYTES] = static_cast<std::byte>(WireError::Unavailable);
                break;
            case Error::Busy:
                frame.bytes[util::HEADER_BYTES] = static_cast<std::byte>(WireError::Busy);
                break;
            case Error::Rejected:
                frame.bytes[util::HEADER_BYTES] = static_cast<std::byte>(WireError::Rejected);
                break;
            case Error::Timeout:
                frame.bytes[util::HEADER_BYTES] = static_cast<std::byte>(WireError::Timeout);
                break;
            case Error::HandlerFailed:
                frame.bytes[util::HEADER_BYTES] = static_cast<std::byte>(WireError::HandlerFailed);
                break;
            case Error::TransportError:
                frame.bytes[util::HEADER_BYTES] = static_cast<std::byte>(WireError::TransportError);
                break;
            default:
                throw std::invalid_argument("Unknown native action error");
        }
        return frame;
    }

    ActionBridge::Error ActionBridge::decodeError(std::span<const std::byte> payload)
    {
        if (payload.size() != 1)
            throw std::invalid_argument("Invalid action error length");
        switch (static_cast<WireError>(payload[0])) {
            case WireError::Unavailable:
                return Error::Unavailable;
            case WireError::Busy:
                return Error::Busy;
            case WireError::Rejected:
                return Error::Rejected;
            case WireError::Timeout:
                return Error::Timeout;
            case WireError::HandlerFailed:
                return Error::HandlerFailed;
            case WireError::TransportError:
                return Error::TransportError;
            default:
                throw std::invalid_argument("Unknown action error");
        }
    }

    std::byte ActionBridge::encodeStatus(Status status)
    {
        switch (status) {
            case Status::Succeeded:
                return std::byte{ 1 };
            case Status::Aborted:
                return std::byte{ 2 };
            case Status::Cancelled:
                return std::byte{ 3 };
            case Status::Rejected:
                return std::byte{ 4 };
        }
        throw std::invalid_argument("Unknown action status");
    }

    ActionBridge::Status ActionBridge::decodeStatus(std::byte status)
    {
        switch (std::to_integer<unsigned>(status)) {
            case 1:
                return Status::Succeeded;
            case 2:
                return Status::Aborted;
            case 3:
                return Status::Cancelled;
            case 4:
                return Status::Rejected;
            default:
                throw std::invalid_argument("Unknown action terminal status");
        }
    }

    void ActionBridge::checkRegistration(std::string_view name) const
    {
        if (name.empty())
            throw std::invalid_argument("Bridge actions require a name");
        if (name.size() > util::MAX_FRAME_BYTES - util::HEADER_BYTES)
            throw std::length_error("Action name exceeds frame capacity");
        const auto duplicate = [name](const auto& route) { return route.name == name; };
        if (std::ranges::any_of(m_outgoingRoutes, duplicate) ||
            std::ranges::any_of(m_incomingRoutes, duplicate))
            throw std::invalid_argument("Bridge action must be registered only once");
        if (m_outgoingRoutes.size() + m_incomingRoutes.size() >= MAX_ACTIONS)
            throw std::length_error("Bridge action capacity reached");
    }

    pnm::coro::Context::Timer ActionBridge::wakeAfter(Clock::duration delay)
    {
        const auto now = Clock::now();
        const auto deadline =
          delay >= Clock::time_point::max() - now ? Clock::time_point::max() : now + delay;
        return m_context.scheduleAt(deadline, [signal = m_wakeup] { signal->notify(); });
    }

    void ActionBridge::poll()
    {
        for (auto& route : m_outgoingRoutes)
            route.poll();
        for (auto& goal : m_incomingGoals) {
            goal.poll();
            if (goal.events->accepted || goal.events->terminal)
                goal.timer = {};
        }
    }

    void ActionBridge::reject(std::uint64_t id, Error error)
    {
        // Separate bounded space lets a full goal table still reject submissions.
        if (m_rejections.size() >= MAX_GOALS)
            throw std::runtime_error("Peer exceeded action rejection capacity");
        m_rejections.push_back({ .id = id, .terminal = makeError(id, error) });
        m_wakeup->notify();
    }

    std::optional<ActionBridge::Frame> ActionBridge::takeFrame(Lane lane)
    {
        for (auto it = m_outgoingGoals.begin(); it != m_outgoingGoals.end();) {
            const bool pending = it->pending();
            if (!pending && it->request) {
                it = m_outgoingGoals.erase(it); // Never sent; no remote execution exists.
                continue;
            }
            if (it->request && it->cancelled()) {
                it->cancel_before_submission();
                it = m_outgoingGoals.erase(it);
                continue;
            }
            if (lane == Lane::Urgent && !it->request && !it->cancel_sent && (!pending || it->cancelled())) {
                it->cancel_sent = true;
                return util::make_frame(Kind::ActionCancel, {}, it->id, 0);
            }
            ++it;
        }
        if (lane == Lane::Urgent && !m_acknowledgements.empty()) {
            const auto id = m_acknowledgements.front();
            m_acknowledgements.pop_front();
            return util::make_frame(Kind::ActionAck, {}, id, 0);
        }
        if (lane == Lane::Urgent)
            return std::nullopt;
        return takeNormalFrame();
    }

    std::optional<ActionBridge::Frame> ActionBridge::takeNormalFrame()
    {
        for (auto& rejection : m_rejections) {
            if (!rejection.sent) {
                rejection.sent = true;
                return rejection.terminal;
            }
        }
        // Rotate between active providers; a busy feedback source cannot starve
        // another goal's acceptance/result. Preserve each goal's event ordering.
        for (std::size_t checked = 0; checked < m_incomingGoals.size(); ++checked) {
            m_nextIncoming %= m_incomingGoals.size();
            auto& goal = m_incomingGoals[m_nextIncoming++];
            auto& events = *goal.events;
            if (std::exchange(events.acceptance_pending, false))
                return util::make_frame(Kind::ActionAccepted, {}, goal.id, 0);
            if (events.feedback)
                return std::exchange(events.feedback, std::nullopt);
            if (events.terminal && !events.terminal_sent) {
                events.terminal_sent = true;
                return events.terminal; // Retain a copy until peer acknowledgement.
            }
        }
        for (auto& goal : m_outgoingGoals) {
            if (goal.request) {
                const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(goal.remaining()).count();
                if (remaining <= 0) {
                    m_wakeup->notify();
                    continue;
                }
                auto frame = *goal.request;
                goal.request.reset();
                util::set_request_budget(frame,
                                       static_cast<std::uint32_t>(std::min<std::uint64_t>(
                                         remaining, std::numeric_limits<std::uint32_t>::max())));
                return frame;
            }
        }
        return std::nullopt;
    }

    void ActionBridge::requestStop()
    {
        if (m_stopping)
            return;
        m_stopping = true;
        for (auto& goal : m_outgoingGoals)
            goal.cancel();
        for (auto& goal : m_incomingGoals)
            goal.cancel();
        m_wakeup->notify();
    }

    bool ActionBridge::drained() const
    {
        return m_outgoingGoals.empty() && m_incomingGoals.empty() && m_rejections.empty() &&
               m_acknowledgements.empty() && m_earlyCancellations.empty();
    }

    void ActionBridge::close(Error error)
    {
        if (m_closed)
            return;
        m_closed = true;
        m_closeError = error;
        for (auto& goal : m_outgoingGoals)
            goal.fail(error);
        for (auto& goal : m_incomingGoals)
            goal.cancel();
        m_outgoingGoals.clear();
        m_incomingGoals.clear();
        // Fail queued proxy requests without invoking application providers.
        for (auto& route : m_outgoingRoutes)
            route.poll();
        m_outgoingRoutes.clear();
        m_incomingRoutes.clear();
        m_rejections.clear();
        m_acknowledgements.clear();
        m_earlyCancellations.clear();
    }

    void ActionBridge::dispatch(const util::IncomingFrame& frame, Lane lane)
    {
        const auto kind = frame.kind;
        const auto required_lane =
          kind == Kind::ActionCancel || kind == Kind::ActionAck ? Lane::Urgent : Lane::Normal;
        if (lane != required_lane)
            throw std::invalid_argument("Action frame on the wrong transport lane");
        if (frame.id == 0)
            throw std::invalid_argument("Action goal ID must be nonzero");
        if (kind == Kind::ActionGoal) {
            dispatchGoal(frame);
            return;
        }
        if (!frame.name.empty() || frame.budget != 0)
            throw std::invalid_argument("Unexpected action lifecycle header fields");
        if (kind == Kind::ActionCancel || kind == Kind::ActionAck)
            dispatchControl(frame);
        else
            dispatchEvent(frame);
    }

    void ActionBridge::dispatchGoal(const util::IncomingFrame& frame)
    {
        const auto& [kind, name, id, budget, payload] = frame;
        if (name.empty() || budget == 0 || id <= m_lastIncoming)
            throw std::invalid_argument("Invalid or repeated action goal");
        m_lastIncoming = id;
        const bool cancelled = std::erase(m_earlyCancellations, id) != 0;
        if (m_stopping) {
            reject(id, Error::Unavailable);
            return;
        }
        const auto route = std::ranges::find(m_incomingRoutes, name, &IncomingRoute::name);
        if (route == m_incomingRoutes.end()) {
            reject(id, Error::Unavailable);
            return;
        }
        if (cancelled) {
            // No provider has received this goal, so cancellation is confirmed.
            // Retain the typed terminal until acknowledged like any rejection.
            if (m_rejections.size() >= MAX_GOALS)
                throw std::runtime_error("Peer exceeded action rejection capacity");
            try {
                m_rejections.push_back({ .id = id, .terminal = route->cancelled(id) });
                m_wakeup->notify();
            } catch (...) {
                reject(id, Error::TransportError);
            }
            return;
        }
        if (m_incomingGoals.size() >= MAX_GOALS) {
            reject(id, Error::Busy);
            return;
        }
        try {
            m_incomingGoals.push_back(route->submit(id, payload, std::chrono::milliseconds(budget)));
        } catch (...) {
            reject(id, Error::HandlerFailed);
        }
    }

    void ActionBridge::dispatchControl(const util::IncomingFrame& frame)
    {
        const auto& [kind, name, id, budget, payload] = frame;
        if (!payload.empty() || (kind == Kind::ActionAck && id > m_lastIncoming))
            throw std::invalid_argument("Invalid action cancellation/acknowledgement");
        const auto goal = std::ranges::find(m_incomingGoals, id, &IncomingGoal::id);
        if (kind == Kind::ActionCancel) {
            if (goal != m_incomingGoals.end())
                goal->cancel();
            else if (id > m_lastIncoming &&
                     std::ranges::find(m_earlyCancellations, id) == m_earlyCancellations.end()) {
                if (m_earlyCancellations.size() >= MAX_GOALS)
                    throw std::runtime_error("Peer exceeded early action cancellation capacity");
                m_earlyCancellations.push_back(id);
            }
        }
        else {
            if (goal != m_incomingGoals.end()) {
                if (!goal->events->terminal_sent)
                    throw std::invalid_argument("Premature action acknowledgement");
                m_incomingGoals.erase(goal);
            }
            const auto rejection = std::ranges::find(m_rejections, id, &Rejection::id);
            if (rejection != m_rejections.end()) {
                if (!rejection->sent)
                    throw std::invalid_argument("Premature action rejection acknowledgement");
                m_rejections.erase(rejection);
            }
        }
        m_wakeup->notify();
    }

    std::pair<std::optional<ActionBridge::Status>, std::optional<ActionBridge::Error>>
    ActionBridge::decodeEvent(const util::IncomingFrame& frame)
    {
        const auto kind = frame.kind;
        const auto payload = frame.payload;
        std::optional<Status> status;
        std::optional<Error> error;
        switch (kind) {
            case Kind::ActionAccepted:
                if (!payload.empty())
                    throw std::invalid_argument("Invalid action acceptance");
                break;
            case Kind::ActionFeedback:
                break;
            case Kind::ActionResult:
                if (payload.empty())
                    throw std::invalid_argument("Missing action terminal status");
                status = decodeStatus(payload[0]);
                break;
            case Kind::ActionError:
                error = decodeError(payload);
                break;
            default:
                throw std::invalid_argument("Unknown action frame kind");
        }
        return { status, error };
    }

    bool ActionBridge::deliverToGoal(OutgoingGoal& goal,
                                     const util::IncomingFrame& frame,
                                     std::optional<Status> status,
                                     std::optional<Error> error)
    {
        const auto kind = frame.kind;
        if (goal.request)
            throw std::invalid_argument("Action event before submission");
        if (kind == Kind::ActionAccepted) {
            if (goal.accepted)
                throw std::invalid_argument("Repeated action acceptance");
            goal.accepted = true;
            goal.timer = {};
            if (!goal.accept())
                goal.cancel(); // Caller may already have timed out.
        }
        else {
            if ((kind == Kind::ActionFeedback || status == Status::Succeeded ||
                 status == Status::Aborted) &&
                !goal.accepted)
                throw std::invalid_argument("Action progress before acceptance");
            if (goal.accepted && (status == Status::Rejected || error == Error::Rejected))
                throw std::invalid_argument("Accepted action cannot be rejected");
            goal.deliver(frame);
            if (kind != Kind::ActionFeedback)
                return true;
        }
        return false;
    }

    void ActionBridge::dispatchEvent(const util::IncomingFrame& frame)
    {
        const auto& [kind, name, id, budget, payload] = frame;
        if (id > m_lastOutgoing)
            throw std::invalid_argument("Action event for unknown goal");
        const auto [status, error] = decodeEvent(frame);
        const auto goal = std::ranges::find(m_outgoingGoals, id, &OutgoingGoal::id);
        if (goal != m_outgoingGoals.end()) {
            if (deliverToGoal(*goal, frame, status, error))
                m_outgoingGoals.erase(goal);
        }

        if (kind == Kind::ActionResult || kind == Kind::ActionError) {
            // A late result still needs acknowledging after the caller timed out
            // or abandoned its handle. No automatic goal replay occurs.
            if (m_acknowledgements.size() >= MAX_GOALS * 2)
                throw std::runtime_error("Peer exceeded action acknowledgement capacity");
            m_acknowledgements.push_back(id);
        }
        m_wakeup->notify();
    }
}
