#pragma once
#include <set>

namespace p2p::util
{
    template<typename T>
    class TopicRoute final : public Route
    {
        Core& core;
        EndpointId id;
        bool outgoing;
        pnm::msg::Topic<T> topic;
        std::optional<pnm::msg::Subscription<T>> subscription;
        pnm::msg::ReadinessRegistration watch;
        std::optional<Bytes> latest;

      public:
        TopicRoute(Core& c, EndpointId i, std::string_view name, bool send)
          : core(c)
          , id(i)
          , outgoing(send)
          , topic(c.bus.topic<T>(name))
        {
            if (send) {
                subscription.emplace(topic.subscribe([this](const T& value) { latest = encode(value); },
                                                     pnm::msg::SubscriptionOptions::latestOnly()));
                watch = pnm::msg::watch(*subscription, core.wakeup());
            }
        }
        void pump() override
        {
            if (subscription)
                subscription->poll();
            if (latest && core.peer && !core.hello_pending &&
                core.send(Frame{ .kind = Kind::Topic, .endpoint = id, .payload = *latest }))
                latest.reset();
        }
        void receive(const Frame& f) override
        {
            if (outgoing || f.kind != Kind::Topic || f.id)
                throw std::invalid_argument("invalid topic frame");
            topic.publish(decode<T>(f.payload));
        }
        void disconnect() override { latest.reset(); }
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
        Core& core;
        EndpointId id;
        bool importing;
        Lane lane;
        pnm::msg::Service<Q, R> service;
        std::optional<pnm::msg::ServiceServer<Q, R>> server;
        pnm::msg::ReadinessRegistration serverWatch;
        std::map<RequestId, std::shared_ptr<Job>> jobs;
        std::set<RequestId> earlyCancel;
        RequestId highest{};
        void reject(const Frame& f, pnm::msg::ServiceError error)
        {
            if (!core.send(Frame{ .kind = Kind::Reply,
                                  .code = static_cast<std::uint8_t>(1 + static_cast<unsigned>(error)),
                                  .endpoint = id,
                                  .id = f.id },
                           Lane::Urgent))
                throw std::runtime_error("p2p rejection channel saturated");
        }

      public:
        ServiceRoute(Core& c, EndpointId i, std::string_view name, bool incoming, Lane l)
          : core(c)
          , id(i)
          , importing(incoming)
          , lane(l)
          , service(c.bus.service<Q, R>(name))
        {
            if (importing) {
                server.emplace(service.serveDeferred([this](const Q& value, pnm::msg::Reply<R> reply) {
                    if (core.state.load() != LinkState::Connected || core.hello_pending) {
                        reply.fail(pnm::msg::ServiceError::Unavailable);
                        return;
                    }
                    if (jobs.size() >= core.options.max_pending) {
                        reply.fail(pnm::msg::ServiceError::Busy);
                        return;
                    }
                    auto job = std::make_shared<Job>();
                    job->id = core.allocateId();
                    job->deadline = Clock::now() + reply.remainingTime();
                    job->frame = {
                        .kind = Kind::Request, .endpoint = id, .id = job->id, .payload = encode(value)
                    };
                    job->reply.emplace(std::move(reply));
                    job->watch = pnm::msg::watch(*job->reply, core.wakeup());
                    jobs.emplace(job->id, std::move(job));
                }, { core.options.max_pending }));
                serverWatch = pnm::msg::watch(*server, core.wakeup());
            }
        }
        Clock::time_point deadline() const override
        {
            auto next = Clock::time_point::max();
            for (auto& [key, j] : jobs)
                if (!j->terminal && !j->cancelled)
                    next = std::min(next, j->deadline);
            return next;
        }
        void pump() override
        {
            if (server)
                server->poll();
            for (auto it = jobs.begin(); it != jobs.end();) {
                auto& j = *it->second;
                if (importing) {
                    if (j.terminal) {
                        if (core.send(Frame{ .kind = Kind::Ack, .endpoint = id, .id = j.id }, Lane::Urgent)) {
                            it = jobs.erase(it);
                            continue;
                        }
                    }
                    else if (!j.reply->pending()) {
                        if (!j.sent) {
                            it = jobs.erase(it);
                            continue;
                        }
                        if (!j.cancelled)
                            j.cancelled = core.send(Frame{ .kind = Kind::Cancel, .endpoint = id, .id = j.id },
                                                    Lane::Urgent);
                    }
                    else if (!j.sent) {
                        j.frame.budget_ms = budget(j.deadline - Clock::now());
                        j.sent = core.send(j.frame, lane);
                    }
                }
                else {
                    if (j.call && !j.terminal)
                        j.call->poll();
                    if (j.terminal && !j.sent)
                        j.sent = core.send(j.frame, lane);
                    if (j.acknowledged) {
                        it = jobs.erase(it);
                        continue;
                    }
                }
                ++it;
            }
        }
        void receive(const Frame& f) override
        {
            if (!f.id)
                throw std::invalid_argument("missing service request ID");
            auto found = jobs.find(f.id);
            if (importing) {
                if (f.kind != Kind::Reply)
                    throw std::invalid_argument("unexpected service reply frame");
                if (found == jobs.end()) {
                    core.send(Frame{ .kind = Kind::Ack, .endpoint = id, .id = f.id }, Lane::Urgent);
                    return;
                }
                auto& j = *found->second;
                if (!j.terminal) {
                    if (f.code == 0)
                        j.reply->respond(decode<R>(f.payload));
                    else if (f.code <= 1 + static_cast<unsigned>(pnm::msg::ServiceError::TransportError))
                        j.reply->fail(static_cast<pnm::msg::ServiceError>(f.code - 1));
                    else
                        throw std::invalid_argument("invalid service error");
                    j.terminal = true;
                }
                return;
            }
            if (f.kind == Kind::Ack) {
                if (found != jobs.end()) {
                    if (!found->second->terminal || !found->second->sent)
                        throw std::invalid_argument("premature p2p acknowledgement");
                    found->second->acknowledged = true;
                }
                return;
            }
            if (f.kind == Kind::Cancel) {
                if (found != jobs.end()) {
                    if (found->second->call)
                        found->second->call->cancel();
                }
                else if (f.id > highest) {
                    if (earlyCancel.size() >= core.options.max_pending)
                        throw std::runtime_error("too many early cancellations");
                    earlyCancel.insert(f.id);
                }
                return;
            }
            if (f.kind != Kind::Request)
                throw std::invalid_argument("unexpected service request frame");
            if (found != jobs.end() || f.id <= highest)
                return;
            highest = f.id;
            if (jobs.size() >= core.options.max_pending) {
                earlyCancel.erase(f.id);
                reject(f, pnm::msg::ServiceError::Busy);
                return;
            }
            auto j = std::make_shared<Job>();
            j->id = f.id;
            j->deadline = Clock::now() + std::chrono::milliseconds(f.budget_ms);
            j->frame = { .kind = Kind::Reply, .endpoint = id, .id = f.id };
            jobs.emplace(f.id, j);
            if (earlyCancel.erase(f.id) || !f.budget_ms) {
                j->frame.code = 1 + static_cast<unsigned>(pnm::msg::ServiceError::Cancelled);
                j->terminal = true;
                return;
            }
            auto weak = std::weak_ptr<Job>(j);
            j->call.emplace(service.request(decode<Q>(f.payload),
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
            j->watch = pnm::msg::watch(*j->call, core.wakeup());
        }
        void disconnect() override
        {
            for (auto& [key, j] : jobs) {
                if (j->reply)
                    j->reply->fail(pnm::msg::ServiceError::TransportError);
                if (j->call)
                    j->call->cancel();
            }
            jobs.clear();
            earlyCancel.clear();
            highest = 0;
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
            bool sent{}, cancelled{}, accepted{}, acceptanceSent{}, terminal{}, acknowledged{};
        };
        Core& core;
        EndpointId id;
        bool importing;
        pnm::msg::Action<G, F, R> action;
        std::optional<pnm::msg::ActionServer<G, F, R>> server;
        pnm::msg::ReadinessRegistration serverWatch;
        std::map<RequestId, std::shared_ptr<Job>> jobs;
        std::set<RequestId> earlyCancel;
        RequestId highest{};

      public:
        ActionRoute(Core& c, EndpointId i, std::string_view name, bool incoming)
          : core(c)
          , id(i)
          , importing(incoming)
          , action(c.bus.action<G, F, R>(name))
        {
            if (importing) {
                server.emplace(
                  action.serveDeferred([this](const G& value, pnm::msg::ActionExecution<F, R> execution) {
                    if (core.state.load() != LinkState::Connected || core.hello_pending) {
                        execution.fail(pnm::msg::ActionError::Unavailable);
                        return;
                    }
                    if (jobs.size() >= core.options.max_pending) {
                        execution.fail(pnm::msg::ActionError::Busy);
                        return;
                    }
                    auto j = std::make_shared<Job>();
                    j->id = core.allocateId();
                    j->deadline = Clock::now() + execution.remainingAcceptanceTime();
                    j->frame = { .kind = Kind::Goal, .endpoint = id, .id = j->id, .payload = encode(value) };
                    j->execution.emplace(std::move(execution));
                    j->watch = pnm::msg::watch(*j->execution, core.wakeup());
                    jobs.emplace(j->id, std::move(j));
                }, { core.options.max_pending }));
                serverWatch = pnm::msg::watch(*server, core.wakeup());
            }
        }
        Clock::time_point deadline() const override
        {
            auto next = Clock::time_point::max();
            for (auto& [key, j] : jobs)
                if (!j->terminal && !j->accepted && !j->cancelled)
                    next = std::min(next, j->deadline);
            return next;
        }
        void pump() override
        {
            if (server)
                server->poll();
            for (auto it = jobs.begin(); it != jobs.end();) {
                auto& j = *it->second;
                if (importing) {
                    if (j.terminal) {
                        if (core.send(Frame{ .kind = Kind::Ack, .endpoint = id, .id = j.id }, Lane::Urgent)) {
                            it = jobs.erase(it);
                            continue;
                        }
                    }
                    else if (j.execution->cancelRequested() || !j.execution->pending()) {
                        if (!j.sent) {
                            j.execution->cancelled(R{});
                            it = jobs.erase(it);
                            continue;
                        }
                        if (!j.cancelled)
                            j.cancelled = core.send(Frame{ .kind = Kind::Cancel, .endpoint = id, .id = j.id },
                                                    Lane::Urgent);
                    }
                    else if (!j.sent) {
                        j.frame.budget_ms = budget(j.deadline - Clock::now());
                        j.sent = core.send(j.frame);
                    }
                }
                else {
                    if (j.goal && !j.terminal)
                        j.goal->poll();
                    if (j.accepted && !j.acceptanceSent)
                        j.acceptanceSent =
                          core.send(Frame{ .kind = Kind::Accepted, .endpoint = id, .id = j.id });
                    if (j.accepted && !j.acceptanceSent) {
                        ++it;
                        continue;
                    }
                    if (j.feedback && !j.terminal &&
                        core.send(Frame{
                          .kind = Kind::Feedback, .endpoint = id, .id = j.id, .payload = *j.feedback }))
                        j.feedback.reset();
                    if (j.terminal && !j.sent)
                        j.sent = core.send(j.frame);
                    if (j.acknowledged) {
                        it = jobs.erase(it);
                        continue;
                    }
                }
                ++it;
            }
        }
        void receive(const Frame& f) override
        {
            if (!f.id)
                throw std::invalid_argument("missing action request ID");
            auto found = jobs.find(f.id);
            if (importing) {
                if (found == jobs.end()) {
                    if (f.kind == Kind::Result)
                        core.send(Frame{ .kind = Kind::Ack, .endpoint = id, .id = f.id }, Lane::Urgent);
                    return;
                }
                auto& j = *found->second;
                if (f.kind == Kind::Accepted) {
                    j.accepted = true;
                    j.execution->accept();
                }
                else if (f.kind == Kind::Feedback) {
                    if (j.accepted && !j.terminal)
                        j.execution->feedback(decode<F>(f.payload));
                }
                else if (f.kind == Kind::Result) {
                    if (!j.terminal) {
                        if (f.code >= 16) {
                            if (f.code > 16 + static_cast<unsigned>(pnm::msg::ActionError::TransportError))
                                throw std::invalid_argument("invalid action error");
                            j.execution->fail(static_cast<pnm::msg::ActionError>(f.code - 16));
                        }
                        else {
                            auto result = decode<R>(f.payload);
                            switch (static_cast<pnm::msg::ActionStatus>(f.code)) {
                                case pnm::msg::ActionStatus::Succeeded:
                                    if (!j.accepted)
                                        throw std::invalid_argument("result before acceptance");
                                    j.execution->succeed(result);
                                    break;
                                case pnm::msg::ActionStatus::Aborted:
                                    if (!j.accepted)
                                        throw std::invalid_argument("result before acceptance");
                                    j.execution->abort(result);
                                    break;
                                case pnm::msg::ActionStatus::Cancelled:
                                    j.execution->cancelled(result);
                                    break;
                                case pnm::msg::ActionStatus::Rejected:
                                    if (j.accepted)
                                        throw std::invalid_argument("rejection after acceptance");
                                    j.execution->reject(result);
                                    break;
                                default:
                                    throw std::invalid_argument("invalid action completion");
                            }
                        }
                        j.terminal = true;
                    }
                }
                else
                    throw std::invalid_argument("unexpected action response");
                return;
            }
            if (f.kind == Kind::Ack) {
                if (found != jobs.end()) {
                    if (!found->second->terminal || !found->second->sent)
                        throw std::invalid_argument("premature p2p acknowledgement");
                    found->second->acknowledged = true;
                }
                return;
            }
            if (f.kind == Kind::Cancel) {
                if (found != jobs.end()) {
                    if (found->second->goal)
                        found->second->goal->requestCancel();
                }
                else if (f.id > highest) {
                    if (earlyCancel.size() >= core.options.max_pending)
                        throw std::runtime_error("too many early cancellations");
                    earlyCancel.insert(f.id);
                }
                return;
            }
            if (f.kind != Kind::Goal)
                throw std::invalid_argument("unexpected action request");
            if (found != jobs.end() || f.id <= highest)
                return;
            highest = f.id;
            if (jobs.size() >= core.options.max_pending) {
                earlyCancel.erase(f.id);
                if (!core.send(Frame{ .kind = Kind::Result,
                                      .code = 16 + static_cast<unsigned>(pnm::msg::ActionError::Busy),
                                      .endpoint = id,
                                      .id = f.id },
                               Lane::Urgent))
                    throw std::runtime_error("p2p rejection channel saturated");
                return;
            }
            auto j = std::make_shared<Job>();
            j->id = f.id;
            j->deadline = Clock::now() + std::chrono::milliseconds(f.budget_ms);
            j->frame = { .kind = Kind::Result, .endpoint = id, .id = f.id };
            jobs.emplace(f.id, j);
            if (earlyCancel.erase(f.id)) {
                j->terminal = true;
                j->frame.code = static_cast<std::uint8_t>(pnm::msg::ActionStatus::Cancelled);
                j->frame.payload = encode(R{});
                return;
            }
            if (!f.budget_ms) {
                j->terminal = true;
                j->frame.code = 16 + static_cast<unsigned>(pnm::msg::ActionError::Timeout);
                return;
            }
            auto weak = std::weak_ptr<Job>(j);
            j->goal.emplace(action.sendGoal(
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
                        p->frame.code = static_cast<std::uint8_t>(16 + static_cast<unsigned>(result.error()));
                }
            } }));
            j->watch = pnm::msg::watch(*j->goal, core.wakeup());
        }
        void disconnect() override
        {
            for (auto& [key, j] : jobs) {
                if (j->execution)
                    j->execution->fail(pnm::msg::ActionError::TransportError);
                if (j->goal)
                    j->goal->requestCancel();
            }
            jobs.clear();
            earlyCancel.clear();
            highest = 0;
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
