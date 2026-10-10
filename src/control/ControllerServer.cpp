#include "ControllerServer.hpp"
#include "runtime/thread.hpp"
#include <deque>

namespace control
{
    using namespace std::chrono_literals;
    using Op = protocol::Operation;
    struct ControllerServer::State : std::enable_shared_from_this<State>
    {
        using Reply = pnm::msg::Reply<protocol::Response>;
        using Execution = pnm::msg::ActionExecution<protocol::Progress, protocol::Outcome>;
        struct Work
        {
            protocol::Request request;
            std::optional<Reply> reply;
            std::optional<Execution> execution;
            std::array<std::uint64_t, 3> epoch{};
        };
        struct Job
        {
            explicit Job(Execution value)
              : execution(std::move(value))
            {
            }
            Execution execution;
            bool group{}, cancelled{};
            ControllerTypes::MotorId motor{};
            ControllerTypes::MotionId id{};
            Completion<MotorResult> completion;
            std::atomic_bool ready{};
            pnm::msg::ReadinessRegistration watch;
        };
        std::shared_ptr<MotionController> controller;
        pnm::msg::Bus& bus;
        pnm::coro::Context& context;
        std::optional<pnm::msg::ServiceServer<protocol::Request, protocol::Response>> requests, stops;
        std::optional<pnm::msg::ActionServer<protocol::Request, protocol::Progress, protocol::Outcome>>
          actions;
        std::array<pnm::msg::ReadinessRegistration, 3> registrations;
        std::mutex mutex;
        std::deque<Work> queue, urgent;
        std::array<std::uint64_t, 3> epoch{};
        std::vector<std::shared_ptr<Job>> jobs;
        runtime::Notification notification;
        std::jthread worker;
        std::atomic_bool closing{}, scheduled{}, connected{}, inhibited{}, lossPending{}, groupActive{};
        State(std::shared_ptr<MotionController> c, pnm::msg::Bus& b, pnm::coro::Context& x)
          : controller(std::move(c))
          , bus(b)
          , context(x)
        {
        }
        void dispatch() noexcept
        {
            if (closing.load() || scheduled.exchange(true))
                return;
            try {
                pnm::coro::co_spawn(
                  context, [self = shared_from_this()](pnm::coro::Context&) -> pnm::coro::Task<void> {
                    self->scheduled.store(false);
                    if (!self->closing.load()) {
                        try {
                            self->stops->poll();
                            self->requests->poll();
                            self->actions->poll();
                        } catch (...) {
                            self->link(p2p::LinkState::ProtocolError);
                        }
                    }
                    co_return;
                });
            } catch (...) {
                scheduled.store(false);
                link(p2p::LinkState::ProtocolError);
            }
        }
        void link(p2p::LinkState value) noexcept
        {
            if (value == p2p::LinkState::Connected) {
                connected.store(true);
                notification.signal();
                return;
            }
            if (value == p2p::LinkState::Starting)
                return;
            connected.store(false);
            inhibited.store(true);
            controller->inhibitRemoteMotion();
            {
                std::scoped_lock lock(mutex);
                for (auto& e : epoch)
                    ++e;
            }
            lossPending.store(true);
            notification.signal();
        }
        void enqueue(Work work, bool priority)
        {
            if (priority && work.request.operation != Op::Stop && work.request.operation != Op::Disable) {
                work.reply->fail(pnm::msg::ServiceError::HandlerFailed);
                return;
            }
            if (closing.load() || !connected.load()) {
                if (work.reply)
                    work.reply->fail(pnm::msg::ServiceError::Unavailable);
                if (work.execution)
                    work.execution->fail(pnm::msg::ActionError::Unavailable);
                return;
            }
            {
                std::scoped_lock lock(mutex);
                auto& destination = priority ? urgent : queue;
                if (destination.size() >= 8) {
                    if (work.reply)
                        work.reply->fail(pnm::msg::ServiceError::Busy);
                    if (work.execution)
                        work.execution->fail(pnm::msg::ActionError::Busy);
                    return;
                }
                if (priority) {
                    auto motor = selection(work.request.motor);
                    if (work.request.operation == Op::Disable || groupActive.load())
                        motor.reset();
                    if (motor)
                        ++epoch[static_cast<unsigned>(*motor)];
                    else
                        for (auto& e : epoch)
                            ++e;
                    controller->requestStop(motor);
                }
                work.epoch = epoch;
                destination.push_back(std::move(work));
            }
            notification.signal();
        }
        static std::optional<ControllerTypes::MotorId> selection(std::uint8_t id)
        {
            if (id == 255)
                return {};
            if (id > 2)
                throw std::invalid_argument("invalid motor");
            return static_cast<ControllerTypes::MotorId>(id);
        }
        static ControllerTypes::MotorId motor(std::uint8_t id)
        {
            const auto selected = selection(id);
            if (!selected)
                throw std::invalid_argument("a motor is required");
            return *selected;
        }
        bool fresh(const Work& w)
        {
            std::scoped_lock lock(mutex);
            if (w.request.motor < 3)
                return w.epoch[w.request.motor] == epoch[w.request.motor];
            return w.epoch == epoch;
        }
        static bool readOnly(Op op)
        {
            return op == Op::Snapshot || op == Op::Drivers || op == Op::Defaults || op == Op::AxisDefaults;
        }
        void service(Work& work)
        {
            auto& reply = *work.reply;
            if (!reply.pending())
                return;
            try {
                const auto& q = work.request;
                p2p::Reader r(q.data);
                p2p::Writer out;
                if (!readOnly(q.operation) && q.operation != Op::Stop && q.operation != Op::Disable &&
                    !fresh(work))
                    throw std::runtime_error("command invalidated by stop");
                if (inhibited.load() && !readOnly(q.operation) && q.operation != Op::Stop &&
                    q.operation != Op::Disable && q.operation != Op::Reset)
                    throw std::runtime_error("communication lost; disable and reset before further control");
                switch (q.operation) {
                    case Op::Snapshot:
                        r.finish();
                        protocol::write(out, controller->planningSnapshot());
                        break;
                    case Op::Drivers:
                        r.finish();
                        protocol::write(out, controller->driverStatus());
                        break;
                    case Op::Enable:
                        r.finish();
                        controller->enable();
                        break;
                    case Op::Disable:
                        r.finish();
                        controller->disable();
                        break;
                    case Op::Stop:
                        r.finish();
                        controller->stop(selection(q.motor));
                        break;
                    case Op::Reset:
                        r.finish();
                        controller->recoverRemoteMotion();
                        if (fresh(work) && connected.load())
                            inhibited.store(false);
                        else
                            controller->inhibitRemoteMotion();
                        break;
                    case Op::InitializeDrivers:
                        r.finish();
                        controller->initializeDrivers();
                        break;
                    case Op::ConfigureDriver: {
                        auto c = protocol::driverConfiguration(r);
                        r.finish();
                        controller->configureDriver(motor(q.motor), c);
                        break;
                    }
                    case Op::ConfigureAxis: {
                        auto c = protocol::conversion(r);
                        r.finish();
                        controller->configureAxis(motor(q.motor), std::move(c));
                        break;
                    }
                    case Op::Defaults:
                        r.finish();
                        protocol::write(out, controller->defaults(motor(q.motor)));
                        break;
                    case Op::SetDefaults: {
                        auto d = protocol::defaults(r);
                        r.finish();
                        controller->setDefaults(motor(q.motor), d);
                        break;
                    }
                    case Op::AxisDefaults:
                        r.finish();
                        protocol::write(out, controller->axisDefaults(motor(q.motor)));
                        break;
                    case Op::SetAxisDefaults: {
                        auto d = protocol::axisDefaults(r);
                        r.finish();
                        controller->setAxisDefaults(motor(q.motor), d);
                        break;
                    }
                    case Op::Velocity: {
                        auto v = pnm::units::AngularVelocity::create(r.real());
                        r.finish();
                        out.integer(controller->setVelocity(motor(q.motor), v));
                        break;
                    }
                    case Op::AxisVelocity: {
                        auto v = protocol::speed(r);
                        r.finish();
                        out.integer(controller->setAxisVelocity(motor(q.motor), v));
                        break;
                    }
                    default:
                        throw std::invalid_argument("operation requires a motion action");
                }
                if (!readOnly(q.operation) && !fresh(work)) {
                    if (inhibited.load())
                        controller->inhibitRemoteMotion();
                    else
                        controller->stop(selection(q.motor));
                    throw std::runtime_error("command interrupted by stop/link loss");
                }
                reply.respond({ true, {}, std::move(out.bytes) });
            } catch (const std::exception& e) {
                reply.respond({ false, std::string(e.what()).substr(0, 255), {} });
            } catch (...) {
                reply.fail(pnm::msg::ServiceError::HandlerFailed);
            }
        }
        void action(Work& work)
        {
            auto execution = std::move(*work.execution);
            if (!execution.pending())
                return;
            if (execution.cancelRequested()) {
                execution.cancelled({ MotorResult::Stopped, {} });
                return;
            }
            if (!fresh(work) || inhibited.load() || !connected.load()) {
                execution.reject();
                return;
            }
            if (jobs.size() >= 8) {
                execution.fail(pnm::msg::ActionError::Busy);
                return;
            }
            auto job = std::make_shared<Job>(std::move(execution));
            try {
                const auto& q = work.request;
                p2p::Reader r(q.data);
                ControllerTypes::Motion single{};
                ControllerTypes::GroupMotion group{};
                job->group = q.operation == Op::ReferenceAll || q.operation == Op::Trajectory;
                if (!job->group)
                    job->motor = motor(q.motor);
                switch (q.operation) {
                    case Op::Move: {
                        auto m = protocol::move(r);
                        r.finish();
                        single = controller->move(job->motor, m);
                        break;
                    }
                    case Op::MoveAxis: {
                        auto m = protocol::axisMove(r);
                        r.finish();
                        single = controller->moveAxis(job->motor, m);
                        break;
                    }
                    case Op::Reference: {
                        auto seek = pnm::units::AngularVelocity::create(r.real());
                        auto latch = pnm::units::AngularVelocity::create(r.real());
                        auto timeout = pnm::units::Time::create(r.real());
                        r.finish();
                        single = controller->reference(job->motor, seek, latch, timeout);
                        break;
                    }
                    case Op::ReferenceAxis: {
                        std::optional<ControllerTypes::AxisSpeed> seek, latch;
                        if (r.boolean())
                            seek = protocol::speed(r);
                        if (r.boolean())
                            latch = protocol::speed(r);
                        auto timeout = pnm::units::Time::create(r.real());
                        r.finish();
                        single = controller->referenceAxis(job->motor, seek, latch, timeout);
                        break;
                    }
                    case Op::ReferenceAll: {
                        auto timeout = pnm::units::Time::create(r.real());
                        r.finish();
                        group = controller->referenceAll(timeout);
                        break;
                    }
                    case Op::Trajectory: {
                        auto t = protocol::trajectory(r);
                        auto timeout = pnm::units::Time::create(r.real());
                        r.finish();
                        group = controller->coordinate(t, timeout);
                        break;
                    }
                    default:
                        throw std::invalid_argument("operation is not a motion action");
                }
                job->completion = job->group ? group.observed : single.observed;
                job->id = job->group ? group.id : single.id;
                if (job->completion.result() == MotorResult::Rejected) {
                    job->execution.reject({ MotorResult::Rejected, "motion rejected by axis admission" });
                    return;
                }
                if (!fresh(work) || !job->execution.accept()) {
                    controller->stop(job->group ? std::nullopt : std::optional(job->motor));
                    job->execution.cancelled({ MotorResult::Stopped, {} });
                    return;
                }
                auto weak = weak_from_this();
                auto weakJob = std::weak_ptr<Job>(job);
                job->watch = pnm::msg::watch(job->execution, [weak]() noexcept {
                    if (auto s = weak.lock())
                        s->notification.signal();
                });
                job->completion.then([weak, weakJob]() noexcept {
                    if (auto j = weakJob.lock())
                        j->ready.store(true);
                    if (auto s = weak.lock())
                        s->notification.signal();
                });
                jobs.push_back(job);
                if (job->group)
                    groupActive.store(true);
            } catch (const std::invalid_argument& e) {
                job->execution.reject({ MotorResult::Rejected, std::string(e.what()).substr(0, 255), true });
            } catch (const std::exception& e) {
                job->execution.reject({ MotorResult::Rejected, std::string(e.what()).substr(0, 255) });
            } catch (...) {
                job->execution.fail(pnm::msg::ActionError::HandlerFailed);
            }
        }
        void completions()
        {
            for (auto it = jobs.begin(); it != jobs.end();) {
                auto& job = **it;
                if (job.execution.cancelRequested() && !job.cancelled) {
                    job.cancelled = true;
                    controller->cancelMotion(job.id, job.group);
                }
                if (job.ready.exchange(false)) {
                    auto result = job.completion.result();
                    if (!result) {
                        ++it;
                        continue;
                    }
                    if (job.group)
                        groupActive.store(false);
                    protocol::Outcome outcome{ *result, {} };
                    if (*result == MotorResult::Completed)
                        job.execution.succeed(outcome);
                    else if (*result == MotorResult::Stopped)
                        job.execution.cancelled(outcome);
                    else
                        job.execution.abort(outcome);
                    it = jobs.erase(it);
                }
                else
                    ++it;
            }
        }
        void telemetry()
        {
            auto snapshot = controller->planningSnapshot();
            for (const auto& j : jobs) {
                const auto i = static_cast<unsigned>(j->motor);
                const auto& a = snapshot.status.axes[i];
                j->execution.feedback(
                  { j->id, snapshot.status.generator.pulses[i], a.position.get(), a.referenced });
            }
            bus.topic<protocol::State>(protocol::StateName).publish({ std::move(snapshot) });
            protocol::FaultState faults{};
            auto drivers = controller->driverStatus();
            for (unsigned i = 0; i < 3; ++i)
                faults.drivers[i] = drivers[i].status;
            faults.link_inhibited = inhibited.load();
            bus.topic<protocol::FaultState>(protocol::FaultName).publish(faults);
        }
        void run(std::stop_token stop) noexcept
        {
            auto next = std::chrono::steady_clock::now();
            while (!stop.stop_requested()) {
                try {
                    if (lossPending.exchange(false))
                        controller->stop();
                    completions();
                    if (std::chrono::steady_clock::now() >= next) {
                        telemetry();
                        next = std::chrono::steady_clock::now() + 20ms;
                    }
                    std::optional<Work> work;
                    {
                        std::scoped_lock lock(mutex);
                        auto& source = urgent.empty() ? queue : urgent;
                        if (!source.empty()) {
                            work.emplace(std::move(source.front()));
                            source.pop_front();
                        }
                    }
                    if (work) {
                        if (work->reply)
                            service(*work);
                        else
                            action(*work);
                        continue;
                    }
                    notification.waitUntil(next);
                } catch (...) {
                    controller->inhibitRemoteMotion();
                    inhibited.store(true);
                    controller->stop();
                    notification.waitUntil(std::chrono::steady_clock::now() + 20ms);
                }
            }
            controller->stop();
            for (auto& job : jobs)
                job->execution.cancelled({ MotorResult::Stopped, {} });
            jobs.clear();
            {
                std::scoped_lock lock(mutex);
                queue.clear();
                urgent.clear();
            }
        }
        void start()
        {
            auto weak = weak_from_this();
            requests.emplace(bus.service<protocol::Request, protocol::Response>(protocol::RequestName)
                               .serveDeferred([weak](const auto& q, Reply reply) {
                if (auto s = weak.lock())
                    s->enqueue({ q, std::move(reply), {}, {} }, false);
            }, { 8 }));
            stops.emplace(bus.service<protocol::Request, protocol::Response>(protocol::StopName)
                            .serveDeferred([weak](const auto& q, Reply reply) {
                if (auto s = weak.lock())
                    s->enqueue({ q, std::move(reply), {}, {} }, true);
            }, { 8 }));
            actions.emplace(
              bus.action<protocol::Request, protocol::Progress, protocol::Outcome>(protocol::ExecuteName)
                .serveDeferred([weak](const auto& q, Execution execution) {
                if (auto s = weak.lock())
                    s->enqueue({ q, {}, std::move(execution), {} }, false);
            }, { 8 }));
            auto wake = [weak]() noexcept {
                if (auto s = weak.lock())
                    s->dispatch();
            };
            registrations[0] = pnm::msg::watch(*requests, wake);
            registrations[1] = pnm::msg::watch(*stops, wake);
            registrations[2] = pnm::msg::watch(*actions, wake);
            worker = runtime::thread::create_jthread(
              { .name = "IO commands", .priority = 6, .stack_size = 65536U },
              [self = shared_from_this()](std::stop_token stop) { self->run(stop); });
        }
        void close()
        {
            closing.store(true);
            actions->requestStop();
            worker.request_stop();
            notification.signal();
            if (worker.joinable())
                worker.join();
            requests->close();
            stops->close();
            actions->close();
            for (auto& r : registrations)
                r.reset();
        }
    };
    ControllerServer::ControllerServer(std::shared_ptr<MotionController> controller,
                                       pnm::msg::Bus& bus,
                                       pnm::coro::Context& context)
      : m_state(std::make_shared<State>(std::move(controller), bus, context))
    {
        m_state->start();
    }
    ControllerServer::~ControllerServer() { m_state->close(); }
    void ControllerServer::linkChanged(p2p::LinkState state) { m_state->link(state); }
}
