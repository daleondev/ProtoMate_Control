#pragma once
#include <functional>
#include <set>

namespace p2p::util
{
    inline constexpr unsigned ACTION_ERROR_OFFSET = 16;

    template<typename T>
    T& checked_state(std::optional<T>& state)
    {
        if (!state)
            throw std::logic_error("missing p2p operation state");
        return *state;
    }

    template<typename T>
    class TopicRoute final : public Route
    {
        std::reference_wrapper<Core> m_core;
        EndpointId m_id;
        bool m_outgoing;
        pnm::msg::Topic<T> m_topic;
        std::optional<pnm::msg::Subscription<T>> m_subscription;
        pnm::msg::ReadinessRegistration m_watch;
        std::optional<Bytes> m_latest;

      public:
        TopicRoute(Core& c, EndpointId i, std::string_view name, bool send)
          : m_core(c)
          , m_id(i)
          , m_outgoing(send)
          , m_topic(c.bus.topic<T>(name))
        {
            if (send) {
                m_subscription.emplace(m_topic.subscribe([this](const T& value) { m_latest = encode(value); },
                                                         pnm::msg::SubscriptionOptions::latestOnly()));
                m_watch = pnm::msg::watch(*m_subscription, m_core.get().wakeup());
            }
        }
        void pump() override
        {
            if (m_subscription)
                m_subscription->poll();
            if (m_latest && m_core.get().peer && !m_core.get().hello_pending &&
                m_core.get().send(Frame{ .kind = Kind::Topic, .endpoint = m_id, .payload = *m_latest }))
                m_latest.reset();
        }
        void receive(const Frame& f) override
        {
            if (m_outgoing || f.kind != Kind::Topic || f.id)
                throw std::invalid_argument("invalid topic frame");
            m_topic.publish(decode<T>(f.payload));
        }
        void disconnect() override { m_latest.reset(); }
    };

    template<typename Q, typename R>
    class ServiceRoute final : public Route
    {
        struct Job
        {
            RequestId id{};
            std::optional<pnm::msg::Reply<R>> reply;
            std::optional<pnm::msg::PendingCall<R>> call;
            pnm::msg::ReadinessRegistration watch;
            Frame frame;
            Clock::time_point deadline{ Clock::time_point::max() };
            bool sent{}, cancelled{}, terminal{}, acknowledged{};
        };
        std::reference_wrapper<Core> m_core;
        EndpointId m_id;
        bool m_importing;
        Lane m_lane;
        pnm::msg::Service<Q, R> m_service;
        std::optional<pnm::msg::ServiceServer<Q, R>> m_server;
        pnm::msg::ReadinessRegistration m_serverWatch;
        std::map<RequestId, std::shared_ptr<Job>> m_jobs;
        std::set<RequestId> m_earlyCancel;
        RequestId m_highest{};
        void reject(const Frame& f, pnm::msg::ServiceError error)
        {
            if (!m_core.get().send(Frame{ .kind = Kind::Reply,
                                          .code = static_cast<std::uint8_t>(1 + static_cast<unsigned>(error)),
                                          .endpoint = m_id,
                                          .id = f.id,
                                          .payload = {} },
                                   Lane::Urgent))
                throw std::runtime_error("p2p rejection channel saturated");
        }

      public:
        ServiceRoute(Core& c, EndpointId i, std::string_view name, bool incoming, Lane l)
          : m_core(c)
          , m_id(i)
          , m_importing(incoming)
          , m_lane(l)
          , m_service(c.bus.service<Q, R>(name))
        {
            if (m_importing) {
                m_server.emplace(m_service.serveDeferred([this](const Q& value, pnm::msg::Reply<R> reply) {
                    if (m_core.get().state.load() != LinkState::Connected || m_core.get().hello_pending) {
                        reply.fail(pnm::msg::ServiceError::Unavailable);
                        return;
                    }
                    if (m_jobs.size() >= m_core.get().options.max_pending) {
                        reply.fail(pnm::msg::ServiceError::Busy);
                        return;
                    }
                    auto job = std::make_shared<Job>();
                    job->id = m_core.get().allocateId();
                    job->deadline = Clock::now() + reply.remainingTime();
                    job->frame = {
                        .kind = Kind::Request, .endpoint = m_id, .id = job->id, .payload = encode(value)
                    };
                    job->reply.emplace(std::move(reply));
                    job->watch = pnm::msg::watch(*job->reply, m_core.get().wakeup());
                    m_jobs.emplace(job->id, std::move(job));
                }, { m_core.get().options.max_pending }));
                m_serverWatch = pnm::msg::watch(*m_server, m_core.get().wakeup());
            }
        }
        Clock::time_point deadline() const override
        {
            auto next = Clock::time_point::max();
            for (auto& [key, j] : m_jobs)
                if (!j->terminal && !j->cancelled)
                    next = std::min(next, j->deadline);
            return next;
        }

      private:
        bool pumpImported(Job& job)
        {
            if (job.terminal)
                return m_core.get().send(
                  Frame{ .kind = Kind::Ack, .endpoint = m_id, .id = job.id, .payload = {} }, Lane::Urgent);
            auto& reply = checked_state(job.reply);
            if (!reply.pending()) {
                if (!job.sent)
                    return true;
                if (!job.cancelled)
                    job.cancelled = m_core.get().send(
                      Frame{ .kind = Kind::Cancel, .endpoint = m_id, .id = job.id, .payload = {} },
                      Lane::Urgent);
            }
            else if (!job.sent) {
                job.frame.budget_ms = budget(job.deadline - Clock::now());
                job.sent = m_core.get().send(job.frame, m_lane);
            }
            return false;
        }
        bool pumpExported(Job& job)
        {
            if (job.call && !job.terminal)
                job.call->poll();
            if (job.terminal && !job.sent)
                job.sent = m_core.get().send(job.frame, m_lane);
            return job.acknowledged;
        }

      public:
        void pump() override
        {
            if (m_server)
                m_server->poll();
            for (auto it = m_jobs.begin(); it != m_jobs.end();) {
                auto& job = *it->second;
                if (m_importing ? pumpImported(job) : pumpExported(job))
                    it = m_jobs.erase(it);
                else
                    ++it;
            }
        }

      private:
        void receiveReply(const Frame& f)
        {
            auto found = m_jobs.find(f.id);
            if (f.kind != Kind::Reply)
                throw std::invalid_argument("unexpected service reply frame");
            if (found == m_jobs.end()) {
                m_core.get().send(Frame{ .kind = Kind::Ack, .endpoint = m_id, .id = f.id, .payload = {} },
                                  Lane::Urgent);
                return;
            }
            auto& j = *found->second;
            if (!j.terminal) {
                if (f.code == 0)
                    checked_state(j.reply).respond(decode<R>(f.payload));
                else if (f.code <= 1 + static_cast<unsigned>(pnm::msg::ServiceError::TransportError))
                    checked_state(j.reply).fail(static_cast<pnm::msg::ServiceError>(f.code - 1));
                else
                    throw std::invalid_argument("invalid service error");
                j.terminal = true;
            }
        }

        bool receiveControl(const Frame& f)
        {
            if (f.kind != Kind::Ack && f.kind != Kind::Cancel)
                return false;
            auto found = m_jobs.find(f.id);
            if (f.kind == Kind::Ack) {
                if (found != m_jobs.end()) {
                    if (!found->second->terminal || !found->second->sent)
                        throw std::invalid_argument("premature p2p acknowledgement");
                    found->second->acknowledged = true;
                }
                return true;
            }
            if (f.kind == Kind::Cancel) {
                if (found != m_jobs.end()) {
                    if (found->second->call)
                        found->second->call->cancel();
                }
                else if (f.id > m_highest) {
                    if (m_earlyCancel.size() >= m_core.get().options.max_pending)
                        throw std::runtime_error("too many early cancellations");
                    m_earlyCancel.insert(f.id);
                }
                return true;
            }
            return false;
        }

      public:
        void receive(const Frame& f) override
        {
            if (!f.id)
                throw std::invalid_argument("missing service request ID");
            if (m_importing) {
                receiveReply(f);
                return;
            }
            if (receiveControl(f))
                return;
            if (f.kind != Kind::Request)
                throw std::invalid_argument("unexpected service request frame");
            auto found = m_jobs.find(f.id);
            if (found != m_jobs.end() || f.id <= m_highest)
                return;
            m_highest = f.id;
            if (m_jobs.size() >= m_core.get().options.max_pending) {
                m_earlyCancel.erase(f.id);
                reject(f, pnm::msg::ServiceError::Busy);
                return;
            }
            auto j = std::make_shared<Job>();
            j->id = f.id;
            j->deadline = Clock::now() + std::chrono::milliseconds(f.budget_ms);
            j->frame = { .kind = Kind::Reply, .endpoint = m_id, .id = f.id, .payload = {} };
            m_jobs.emplace(f.id, j);
            if (m_earlyCancel.erase(f.id) || !f.budget_ms) {
                j->frame.code = 1 + static_cast<unsigned>(pnm::msg::ServiceError::Cancelled);
                j->terminal = true;
                return;
            }
            auto weak = std::weak_ptr<Job>(j);
            j->call.emplace(m_service.request(decode<Q>(f.payload),
                                              std::chrono::milliseconds(f.budget_ms),
                                              [weak](pnm::msg::ServiceResult<R> result) {
                if (auto job = weak.lock()) {
                    job->terminal = true;
                    if (result)
                        job->frame.payload = encode(*result);
                    else
                        job->frame.code =
                          static_cast<std::uint8_t>(1 + static_cast<unsigned>(result.error()));
                }
            }));
            j->watch = pnm::msg::watch(*j->call, m_core.get().wakeup());
        }
        void disconnect() override
        {
            for (auto& [key, j] : m_jobs) {
                if (j->reply)
                    j->reply->fail(pnm::msg::ServiceError::TransportError);
                if (j->call)
                    j->call->cancel();
            }
            m_jobs.clear();
            m_earlyCancel.clear();
            m_highest = 0;
        }
    };

    template<typename G, typename F, typename R>
    class ActionRoute final : public Route
    {
        struct Job
        {
            RequestId id{};
            std::optional<pnm::msg::ActionExecution<F, R>> execution;
            std::optional<pnm::msg::PendingGoal<F, R>> goal;
            pnm::msg::ReadinessRegistration watch;
            Frame frame;
            std::optional<Bytes> feedback;
            Clock::time_point deadline{ Clock::time_point::max() };
            bool sent{}, cancelled{}, accepted{}, acceptance_sent{}, terminal{}, acknowledged{};
        };
        std::reference_wrapper<Core> m_core;
        EndpointId m_id;
        bool m_importing;
        pnm::msg::Action<G, F, R> m_action;
        std::optional<pnm::msg::ActionServer<G, F, R>> m_server;
        pnm::msg::ReadinessRegistration m_serverWatch;
        std::map<RequestId, std::shared_ptr<Job>> m_jobs;
        std::set<RequestId> m_earlyCancel;
        RequestId m_highest{};

      public:
        ActionRoute(Core& c, EndpointId i, std::string_view name, bool incoming)
          : m_core(c)
          , m_id(i)
          , m_importing(incoming)
          , m_action(c.bus.action<G, F, R>(name))
        {
            if (m_importing) {
                m_server.emplace(
                  m_action.serveDeferred([this](const G& value, pnm::msg::ActionExecution<F, R> execution) {
                    if (m_core.get().state.load() != LinkState::Connected || m_core.get().hello_pending) {
                        execution.fail(pnm::msg::ActionError::Unavailable);
                        return;
                    }
                    if (m_jobs.size() >= m_core.get().options.max_pending) {
                        execution.fail(pnm::msg::ActionError::Busy);
                        return;
                    }
                    auto j = std::make_shared<Job>();
                    j->id = m_core.get().allocateId();
                    j->deadline = Clock::now() + execution.remainingAcceptanceTime();
                    j->frame = {
                        .kind = Kind::Goal, .endpoint = m_id, .id = j->id, .payload = encode(value)
                    };
                    j->execution.emplace(std::move(execution));
                    j->watch = pnm::msg::watch(*j->execution, m_core.get().wakeup());
                    m_jobs.emplace(j->id, std::move(j));
                }, { m_core.get().options.max_pending }));
                m_serverWatch = pnm::msg::watch(*m_server, m_core.get().wakeup());
            }
        }
        Clock::time_point deadline() const override
        {
            auto next = Clock::time_point::max();
            for (auto& [key, j] : m_jobs)
                if (!j->terminal && !j->accepted && !j->cancelled)
                    next = std::min(next, j->deadline);
            return next;
        }

      private:
        bool pumpImported(Job& job)
        {
            if (job.terminal)
                return m_core.get().send(
                  Frame{ .kind = Kind::Ack, .endpoint = m_id, .id = job.id, .payload = {} }, Lane::Urgent);
            auto& execution = checked_state(job.execution);
            if (execution.cancelRequested() || !execution.pending()) {
                if (!job.sent) {
                    execution.cancelled(R{});
                    return true;
                }
                if (!job.cancelled)
                    job.cancelled = m_core.get().send(
                      Frame{ .kind = Kind::Cancel, .endpoint = m_id, .id = job.id, .payload = {} },
                      Lane::Urgent);
            }
            else if (!job.sent) {
                job.frame.budget_ms = budget(job.deadline - Clock::now());
                job.sent = m_core.get().send(job.frame);
            }
            return false;
        }
        bool pumpExported(Job& job)
        {
            if (job.goal && !job.terminal)
                job.goal->poll();
            if (job.accepted && !job.acceptance_sent)
                job.acceptance_sent = m_core.get().send(
                  Frame{ .kind = Kind::Accepted, .endpoint = m_id, .id = job.id, .payload = {} });
            if (job.accepted && !job.acceptance_sent)
                return false;
            if (job.feedback && !job.terminal &&
                m_core.get().send(
                  Frame{ .kind = Kind::Feedback, .endpoint = m_id, .id = job.id, .payload = *job.feedback }))
                job.feedback.reset();
            if (job.terminal && !job.sent)
                job.sent = m_core.get().send(job.frame);
            return job.acknowledged;
        }
        void complete(Job& job, const Frame& frame)
        {
            auto& execution = checked_state(job.execution);
            if (frame.code >= ACTION_ERROR_OFFSET) {
                if (frame.code >
                    ACTION_ERROR_OFFSET + static_cast<unsigned>(pnm::msg::ActionError::TransportError))
                    throw std::invalid_argument("invalid action error");
                execution.fail(static_cast<pnm::msg::ActionError>(frame.code - ACTION_ERROR_OFFSET));
                return;
            }
            auto result = decode<R>(frame.payload);
            switch (static_cast<pnm::msg::ActionStatus>(frame.code)) {
                case pnm::msg::ActionStatus::Succeeded:
                    if (!job.accepted)
                        throw std::invalid_argument("result before acceptance");
                    execution.succeed(result);
                    break;
                case pnm::msg::ActionStatus::Aborted:
                    if (!job.accepted)
                        throw std::invalid_argument("result before acceptance");
                    execution.abort(result);
                    break;
                case pnm::msg::ActionStatus::Cancelled:
                    execution.cancelled(result);
                    break;
                case pnm::msg::ActionStatus::Rejected:
                    if (job.accepted)
                        throw std::invalid_argument("rejection after acceptance");
                    execution.reject(result);
                    break;
                default:
                    throw std::invalid_argument("invalid action completion");
            }
        }
        void receiveResponse(const Frame& frame)
        {
            auto found = m_jobs.find(frame.id);
            if (found == m_jobs.end()) {
                if (frame.kind == Kind::Result)
                    m_core.get().send(
                      Frame{ .kind = Kind::Ack, .endpoint = m_id, .id = frame.id, .payload = {} },
                      Lane::Urgent);
                return;
            }
            auto& job = *found->second;
            switch (frame.kind) {
                case Kind::Accepted:
                    job.accepted = true;
                    checked_state(job.execution).accept();
                    break;
                case Kind::Feedback:
                    if (job.accepted && !job.terminal)
                        checked_state(job.execution).feedback(decode<F>(frame.payload));
                    break;
                case Kind::Result:
                    if (!job.terminal) {
                        complete(job, frame);
                        job.terminal = true;
                    }
                    break;
                default:
                    throw std::invalid_argument("unexpected action response");
            }
        }

      public:
        void pump() override
        {
            if (m_server)
                m_server->poll();
            for (auto it = m_jobs.begin(); it != m_jobs.end();) {
                auto& job = *it->second;
                if (m_importing ? pumpImported(job) : pumpExported(job))
                    it = m_jobs.erase(it);
                else
                    ++it;
            }
        }

      private:
        bool receiveControl(const Frame& f)
        {
            if (f.kind != Kind::Ack && f.kind != Kind::Cancel)
                return false;
            auto found = m_jobs.find(f.id);
            if (f.kind == Kind::Ack) {
                if (found != m_jobs.end()) {
                    if (!found->second->terminal || !found->second->sent)
                        throw std::invalid_argument("premature p2p acknowledgement");
                    found->second->acknowledged = true;
                }
                return true;
            }
            if (f.kind == Kind::Cancel) {
                if (found != m_jobs.end()) {
                    if (found->second->goal)
                        found->second->goal->requestCancel();
                }
                else if (f.id > m_highest) {
                    if (m_earlyCancel.size() >= m_core.get().options.max_pending)
                        throw std::runtime_error("too many early cancellations");
                    m_earlyCancel.insert(f.id);
                }
                return true;
            }
            return false;
        }

      public:
        void receive(const Frame& f) override
        {
            if (!f.id)
                throw std::invalid_argument("missing action request ID");
            if (m_importing) {
                receiveResponse(f);
                return;
            }
            if (receiveControl(f))
                return;
            if (f.kind != Kind::Goal)
                throw std::invalid_argument("unexpected action request");
            auto found = m_jobs.find(f.id);
            if (found != m_jobs.end() || f.id <= m_highest)
                return;
            m_highest = f.id;
            if (m_jobs.size() >= m_core.get().options.max_pending) {
                m_earlyCancel.erase(f.id);
                if (!m_core.get().send(
                      Frame{ .kind = Kind::Result,
                             .code = ACTION_ERROR_OFFSET + static_cast<unsigned>(pnm::msg::ActionError::Busy),
                             .endpoint = m_id,
                             .id = f.id,
                             .payload = {} },
                      Lane::Urgent))
                    throw std::runtime_error("p2p rejection channel saturated");
                return;
            }
            auto j = std::make_shared<Job>();
            j->id = f.id;
            j->deadline = Clock::now() + std::chrono::milliseconds(f.budget_ms);
            j->frame = { .kind = Kind::Result, .endpoint = m_id, .id = f.id, .payload = {} };
            m_jobs.emplace(f.id, j);
            if (m_earlyCancel.erase(f.id)) {
                j->terminal = true;
                j->frame.code = static_cast<std::uint8_t>(pnm::msg::ActionStatus::Cancelled);
                j->frame.payload = encode(R{});
                return;
            }
            if (!f.budget_ms) {
                j->terminal = true;
                j->frame.code = ACTION_ERROR_OFFSET + static_cast<unsigned>(pnm::msg::ActionError::Timeout);
                return;
            }
            auto weak = std::weak_ptr<Job>(j);
            j->goal.emplace(m_action.sendGoal(
              decode<G>(f.payload), { std::chrono::milliseconds(f.budget_ms) }, { .on_accepted = [weak] {
                if (auto p = weak.lock())
                    p->accepted = true;
            }, .on_feedback = [weak](const F& value) {
                if (auto p = weak.lock())
                    p->feedback = encode(value);
            }, .on_result = [weak](pnm::msg::ActionResult<R> result) {
                if (auto p = weak.lock()) {
                    p->terminal = true;
                    p->feedback.reset();
                    if (result) {
                        p->frame.code = static_cast<std::uint8_t>(result->status);
                        p->frame.payload = encode(result->value);
                    }
                    else
                        p->frame.code = static_cast<std::uint8_t>(ACTION_ERROR_OFFSET +
                                                                  static_cast<unsigned>(result.error()));
                }
            } }));
            j->watch = pnm::msg::watch(*j->goal, m_core.get().wakeup());
        }
        void disconnect() override
        {
            for (auto& [key, j] : m_jobs) {
                if (j->execution)
                    j->execution->fail(pnm::msg::ActionError::TransportError);
                if (j->goal)
                    j->goal->requestCancel();
            }
            m_jobs.clear();
            m_earlyCancel.clear();
            m_highest = 0;
        }
    };
}

namespace p2p
{
    template<typename T>
    void Bridge::publishTopic(EndpointId id, std::string_view name)
    {
        m_core->registerRoute(id, std::make_unique<util::TopicRoute<T>>(*m_core, id, name, true));
    }
    template<typename T>
    void Bridge::receiveTopic(EndpointId id, std::string_view name)
    {
        m_core->registerRoute(id, std::make_unique<util::TopicRoute<T>>(*m_core, id, name, false));
    }
    template<typename Q, typename R>
    void Bridge::importService(EndpointId id, std::string_view name, Lane lane)
    {
        m_core->registerRoute(id, std::make_unique<util::ServiceRoute<Q, R>>(*m_core, id, name, true, lane));
    }
    template<typename Q, typename R>
    void Bridge::exportService(EndpointId id, std::string_view name, Lane lane)
    {
        m_core->registerRoute(id, std::make_unique<util::ServiceRoute<Q, R>>(*m_core, id, name, false, lane));
    }
    template<typename G, typename F, typename R>
    void Bridge::importAction(EndpointId id, std::string_view name)
    {
        m_core->registerRoute(id, std::make_unique<util::ActionRoute<G, F, R>>(*m_core, id, name, true));
    }
    template<typename G, typename F, typename R>
    void Bridge::exportAction(EndpointId id, std::string_view name)
    {
        m_core->registerRoute(id, std::make_unique<util::ActionRoute<G, F, R>>(*m_core, id, name, false));
    }
}
