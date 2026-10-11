#include "ActionBridge.hpp"

#include <algorithm>

namespace p2p
{
    ActionBridge::ActionBridge(pnm::msg::Bus& bus, pnm::coro::Context& context,
                               std::shared_ptr<util::OutgoingWakeup> wakeup, std::stop_token stop)
      : m_bus(bus), m_context(context), m_wakeup(std::move(wakeup)), m_stop(stop)
    {
    }

    ActionBridge::~ActionBridge() { close(Error::Unavailable); }

    ActionBridge::Frame ActionBridge::makeError(std::uint64_t id, Error error)
    {
        auto frame = util::makeFrame(Kind::ActionError, {}, id, 1);
        switch (error) {
        case Error::Unavailable: frame.bytes[util::HEADER_BYTES] = std::byte{1}; break;
        case Error::Busy: frame.bytes[util::HEADER_BYTES] = std::byte{2}; break;
        case Error::Rejected: frame.bytes[util::HEADER_BYTES] = std::byte{3}; break;
        case Error::Timeout: frame.bytes[util::HEADER_BYTES] = std::byte{4}; break;
        case Error::HandlerFailed: frame.bytes[util::HEADER_BYTES] = std::byte{5}; break;
        case Error::TransportError: frame.bytes[util::HEADER_BYTES] = std::byte{6}; break;
        default: throw std::invalid_argument("Unknown native action error");
        }
        return frame;
    }

    ActionBridge::Error ActionBridge::decodeError(std::span<const std::byte> payload)
    {
        if (payload.size() != 1)
            throw std::invalid_argument("Invalid action error length");
        switch (std::to_integer<unsigned>(payload[0])) {
        case 1: return Error::Unavailable;
        case 2: return Error::Busy;
        case 3: return Error::Rejected;
        case 4: return Error::Timeout;
        case 5: return Error::HandlerFailed;
        case 6: return Error::TransportError;
        default: throw std::invalid_argument("Unknown action error");
        }
    }

    std::byte ActionBridge::encodeStatus(Status status)
    {
        switch (status) {
        case Status::Succeeded: return std::byte{1};
        case Status::Aborted: return std::byte{2};
        case Status::Cancelled: return std::byte{3};
        case Status::Rejected: return std::byte{4};
        }
        throw std::invalid_argument("Unknown action status");
    }

    ActionBridge::Status ActionBridge::decodeStatus(std::byte status)
    {
        switch (std::to_integer<unsigned>(status)) {
        case 1: return Status::Succeeded;
        case 2: return Status::Aborted;
        case 3: return Status::Cancelled;
        case 4: return Status::Rejected;
        default: throw std::invalid_argument("Unknown action terminal status");
        }
    }

    void ActionBridge::checkRegistration(std::string_view name) const
    {
        if (name.empty())
            throw std::invalid_argument("Bridge actions require a name");
        if (name.size() > util::MAX_FRAME_BYTES - util::HEADER_BYTES)
            throw std::length_error("Action name exceeds frame capacity");
        const auto duplicate = [name](const auto& route) { return route.name == name; };
        if (std::ranges::any_of(m_outgoingRoutes, duplicate) || std::ranges::any_of(m_incomingRoutes, duplicate))
            throw std::invalid_argument("Bridge action must be registered only once");
        if (m_outgoingRoutes.size() + m_incomingRoutes.size() >= MAX_ACTIONS)
            throw std::length_error("Bridge action capacity reached");
    }

    pnm::coro::Context::Timer ActionBridge::wakeAfter(Clock::duration delay)
    {
        const auto now = Clock::now();
        const auto deadline = delay >= Clock::time_point::max() - now ? Clock::time_point::max() : now + delay;
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
        m_rejections.push_back({id, makeError(id, error)});
        m_wakeup->notify();
    }

    std::optional<ActionBridge::Frame> ActionBridge::takeFrame()
    {
        for (auto it = m_outgoingGoals.begin(); it != m_outgoingGoals.end();) {
            const bool pending = it->pending();
            if (!pending && it->request) {
                it = m_outgoingGoals.erase(it); // Never sent; no remote execution exists.
                continue;
            }
            if (it->request && it->cancelled()) {
                it->cancelBeforeSubmission();
                it = m_outgoingGoals.erase(it);
                continue;
            }
            if (!it->request && !it->cancelSent && (!pending || it->cancelled())) {
                it->cancelSent = true;
                return util::makeFrame(Kind::ActionCancel, {}, it->id, 0);
            }
            ++it;
        }
        if (!m_acknowledgements.empty()) {
            const auto id = m_acknowledgements.front();
            m_acknowledgements.pop_front();
            return util::makeFrame(Kind::ActionAck, {}, id, 0);
        }
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
            if (std::exchange(events.acceptancePending, false))
                return util::makeFrame(Kind::ActionAccepted, {}, goal.id, 0);
            if (events.feedback)
                return std::exchange(events.feedback, std::nullopt);
            if (events.terminal && !events.terminalSent) {
                events.terminalSent = true;
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
                auto frame = std::exchange(goal.request, std::nullopt);
                util::setRequestBudget(*frame, static_cast<std::uint32_t>(
                  std::min<std::uint64_t>(remaining, std::numeric_limits<std::uint32_t>::max())));
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
               m_acknowledgements.empty();
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
    }

    void ActionBridge::dispatch(const util::IncomingFrame& frame)
    {
        const auto& [kind, name, id, budget, payload] = frame;
        if (id == 0)
            throw std::invalid_argument("Action goal ID must be nonzero");
        if (kind == Kind::ActionGoal) {
            if (name.empty() || budget == 0 || id <= m_lastIncoming)
                throw std::invalid_argument("Invalid or repeated action goal");
            m_lastIncoming = id;
            if (m_stopping) {
                reject(id, Error::Unavailable);
                return;
            }
            const auto route = std::ranges::find(m_incomingRoutes, name, &IncomingRoute::name);
            if (route == m_incomingRoutes.end()) {
                reject(id, Error::Unavailable);
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
            return;
        }
        if (!name.empty() || budget != 0)
            throw std::invalid_argument("Unexpected action lifecycle header fields");
        if (kind == Kind::ActionCancel || kind == Kind::ActionAck) {
            if (!payload.empty() || id > m_lastIncoming)
                throw std::invalid_argument("Invalid action cancellation/acknowledgement");
            const auto goal = std::ranges::find(m_incomingGoals, id, &IncomingGoal::id);
            if (kind == Kind::ActionCancel) {
                if (goal != m_incomingGoals.end())
                    goal->cancel();
            } else {
                if (goal != m_incomingGoals.end()) {
                    if (!goal->events->terminalSent)
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
            return;
        }
        if (id > m_lastOutgoing)
            throw std::invalid_argument("Action event for unknown goal");
        std::optional<Status> status;
        std::optional<Error> error;
        switch (kind) {
        case Kind::ActionAccepted:
            if (!payload.empty()) throw std::invalid_argument("Invalid action acceptance");
            break;
        case Kind::ActionFeedback: break;
        case Kind::ActionResult:
            if (payload.empty()) throw std::invalid_argument("Missing action terminal status");
            status = decodeStatus(payload[0]);
            break;
        case Kind::ActionError: error = decodeError(payload); break;
        default: throw std::invalid_argument("Unknown action frame kind");
        }
        const auto goal = std::ranges::find(m_outgoingGoals, id, &OutgoingGoal::id);
        if (goal != m_outgoingGoals.end()) {
            if (goal->request)
                throw std::invalid_argument("Action event before submission");
            if (kind == Kind::ActionAccepted) {
                if (goal->accepted) throw std::invalid_argument("Repeated action acceptance");
                goal->accepted = true;
                goal->timer = {};
                if (!goal->accept()) goal->cancel(); // Caller may already have timed out.
            } else {
                if ((kind == Kind::ActionFeedback || status == Status::Succeeded || status == Status::Aborted) &&
                    !goal->accepted)
                    throw std::invalid_argument("Action progress before acceptance");
                if (goal->accepted && (status == Status::Rejected || error == Error::Rejected))
                    throw std::invalid_argument("Accepted action cannot be rejected");
                goal->deliver(frame);
                if (kind != Kind::ActionFeedback)
                    m_outgoingGoals.erase(goal);
            }
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
