#include "ControllerServer.hpp"
#include "runtime/thread.hpp"
#include <deque>

namespace
{
    constexpr std::size_t REQUEST_CAPACITY{ 8U };
    constexpr int IO_WORKER_PRIORITY{ 6 };
    constexpr std::size_t IO_WORKER_STACK_SIZE{ 65536U };
}

namespace control
{
    using namespace std::chrono_literals;
    using Op = messages::Operation;
    struct ControllerServer::State : std::enable_shared_from_this<State>
    {
        using Reply = pnm::msg::Reply<messages::Response>;
        using Execution = pnm::msg::ActionExecution<messages::Progress, messages::Outcome>;
        struct Work
        {
            messages::Request request;
            std::optional<Reply> reply;
            std::optional<Execution> execution;
            std::array<std::uint64_t, 3> epoch{};
            std::optional<messages::Goal> goal;
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
            std::atomic_bool ready;
            pnm::msg::ReadinessRegistration watch;
        };
        std::shared_ptr<MotionController> controller;
        pnm::msg::Bus& bus;
        pnm::coro::Context& context;
        std::optional<pnm::msg::ServiceServer<messages::Request, messages::Response>> requests, stops;
        std::optional<pnm::msg::ActionServer<messages::Goal, messages::Progress, messages::Outcome>>
          actions;
        std::array<pnm::msg::ReadinessRegistration, 3> registrations;
        std::mutex mutex;
        std::deque<Work> queue, urgent;
        std::array<std::uint64_t, 3> epoch{};
        std::vector<std::shared_ptr<Job>> jobs;
        runtime::Notification notification;
        std::jthread worker;
        std::atomic_bool closing, scheduled, connected, inhibited, loss_pending, group_active;
        State(std::shared_ptr<MotionController> c, pnm::msg::Bus& b, pnm::coro::Context& x)
          : controller(std::move(c))
          , bus(b)
          , context(x)
        {
        }
        // Clang diagnoses the compiler's implicit static promise-hook calls.
        // NOLINTBEGIN(readability-static-accessed-through-instance)
        static pnm::coro::Task<void> pollOnContext(std::shared_ptr<State> self)
        {
            self->scheduled.store(false);
            if (!self->closing.load()) {
                try {
                    if (!self->stops || !self->requests || !self->actions)
                        throw std::logic_error("controller endpoints are not registered");
                    auto& stops = *self->stops;
                    auto& requests = *self->requests;
                    auto& actions = *self->actions;
                    stops.poll();
                    requests.poll();
                    actions.poll();
                } catch (...) {
                    self->link({ p2p::ConnectionState::Failed, p2p::ConnectionReason::InternalError });
                }
            }
            co_return;
        }
        // NOLINTEND(readability-static-accessed-through-instance)
        void dispatch() noexcept
        {
            if (closing.load() || scheduled.exchange(true))
                return;
            try {
                pnm::coro::co_spawn(context, [self = shared_from_this()](pnm::coro::Context& /*context*/) {
                    return pollOnContext(self);
                });
            } catch (...) {
                scheduled.store(false);
                link({ p2p::ConnectionState::Failed, p2p::ConnectionReason::InternalError });
            }
        }
        void link(p2p::ConnectionStatus value) noexcept
        {
            if (value.state == p2p::ConnectionState::Connected) {
                connected.store(true);
                notification.signal();
                return;
            }
            if (value.state == p2p::ConnectionState::Connecting)
                return;
            connected.store(false);
            inhibited.store(true);
            controller->inhibitRemoteMotion();
            {
                std::scoped_lock lock(mutex);
                for (auto& e : epoch)
                    ++e;
            }
            loss_pending.store(true);
            notification.signal();
        }
        void enqueue(Work work, bool priority)
        {
            if (priority && work.request.operation != Op::Stop && work.request.operation != Op::Disable) {
                if (work.reply)
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
                if (destination.size() >= REQUEST_CAPACITY) {
                    if (work.reply)
                        work.reply->fail(pnm::msg::ServiceError::Busy);
                    if (work.execution)
                        work.execution->fail(pnm::msg::ActionError::Busy);
                    return;
                }
                if (priority) {
                    auto motor = selection(work.request.motor);
                    if (work.request.operation == Op::Disable || group_active.load())
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
            if (id == messages::ALL_MOTORS)
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
            if (!work.reply)
                throw std::logic_error("service work has no reply");
            auto& reply = *work.reply;
            if (!reply.pending())
                return;
            try {
                const auto& q = work.request;
                messages::Response response{};
                if (!readOnly(q.operation) && q.operation != Op::Stop && q.operation != Op::Disable &&
                    !fresh(work))
                    throw std::runtime_error("command invalidated by stop");
                if (inhibited.load() && !readOnly(q.operation) && q.operation != Op::Stop &&
                    q.operation != Op::Disable && q.operation != Op::Reset)
                    throw std::runtime_error("communication lost; disable and reset before further control");
                switch (q.operation) {
                    case Op::Snapshot:
                        response.state = messages::State(controller->planningSnapshot());
                        break;
                    case Op::Drivers: {
                        const auto drivers = controller->driverStatus();
                        for (std::size_t i = 0; i < drivers.size(); ++i)
                            response.drivers[i] = messages::DriverStatus(drivers[i]);
                        break;
                    }
                    case Op::Enable:
                        controller->enable();
                        break;
                    case Op::Disable:
                        controller->disable();
                        break;
                    case Op::Stop:
                        controller->stop(selection(q.motor));
                        break;
                    case Op::Reset:
                        controller->recoverRemoteMotion();
                        if (fresh(work) && connected.load())
                            inhibited.store(false);
                        else
                            controller->inhibitRemoteMotion();
                        break;
                    case Op::InitializeDrivers:
                        controller->initializeDrivers();
                        break;
                    case Op::ConfigureDriver:
                        controller->configureDriver(motor(q.motor), q.driver);
                        break;
                    case Op::ConfigureAxis:
                        if (!q.conversion)
                            throw std::invalid_argument("axis conversion is required");
                        controller->configureAxis(motor(q.motor), *q.conversion);
                        break;
                    case Op::Defaults:
                        response.state.defaults.at(static_cast<unsigned>(motor(q.motor))) =
                          controller->defaults(motor(q.motor));
                        break;
                    case Op::SetDefaults:
                        controller->setDefaults(motor(q.motor), q.defaults);
                        break;
                    case Op::AxisDefaults:
                        response.axis_defaults = controller->axisDefaults(motor(q.motor));
                        break;
                    case Op::SetAxisDefaults:
                        controller->setAxisDefaults(motor(q.motor), q.axis_defaults);
                        break;
                    case Op::Velocity:
                        response.pulses = controller->setVelocity(motor(q.motor), q.velocity);
                        break;
                    case Op::AxisVelocity:
                        response.pulses = controller->setAxisVelocity(motor(q.motor), q.axis_velocity);
                        break;
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
                reply.respond(std::move(response));
            } catch (const std::exception& e) {
                messages::Response response{};
                response.success = false;
                response.error = std::string(e.what()).substr(0, messages::MAX_ERROR_TEXT);
                reply.respond(std::move(response));
            } catch (...) {
                reply.fail(pnm::msg::ServiceError::HandlerFailed);
            }
        }
        void submitAction(const messages::Goal& request, const std::shared_ptr<Job>& job)
        {
            const auto& q = request;
            ControllerTypes::Motion single{};
            ControllerTypes::GroupMotion group{};
            job->group = q.operation == Op::ReferenceAll || q.operation == Op::Trajectory;
            if (!job->group)
                job->motor = motor(q.motor);
            switch (q.operation) {
                case Op::Move:
                    single = controller->move(job->motor, q.move);
                    break;
                case Op::MoveAxis:
                    single = controller->moveAxis(job->motor, q.axis_move);
                    break;
                case Op::Reference:
                    single = controller->reference(job->motor, q.seek, q.latch, q.timeout);
                    break;
                case Op::ReferenceAxis:
                    single = controller->referenceAxis(job->motor, q.axis_seek, q.axis_latch, q.timeout);
                    break;
                case Op::ReferenceAll:
                    group = controller->referenceAll(q.timeout);
                    break;
                case Op::Trajectory:
                    group = controller->coordinate(q.trajectory, q.timeout);
                    break;
                default:
                    throw std::invalid_argument("operation is not a motion action");
            }
            job->completion = job->group ? group.observed : single.observed;
            job->id = job->group ? group.id : single.id;
        }
        void action(Work& work)
        {
            if (!work.execution || !work.goal)
                throw std::logic_error("action work has no execution or goal");
            auto execution = std::move(*work.execution);
            if (!execution.pending())
                return;
            if (execution.cancelRequested()) {
                execution.cancelled({ .result = MotorResult::Stopped, .error = {} });
                return;
            }
            if (!fresh(work) || inhibited.load() || !connected.load()) {
                execution.reject();
                return;
            }
            if (jobs.size() >= REQUEST_CAPACITY) {
                execution.fail(pnm::msg::ActionError::Busy);
                return;
            }
            auto job = std::make_shared<Job>(std::move(execution));
            try {
                submitAction(*work.goal, job);
                if (job->completion.result() == MotorResult::Rejected) {
                    job->execution.reject(
                      { .result = MotorResult::Rejected, .error = "motion rejected by axis admission" });
                    return;
                }
                if (!fresh(work) || !job->execution.accept()) {
                    controller->stop(job->group ? std::nullopt : std::optional(job->motor));
                    job->execution.cancelled({ .result = MotorResult::Stopped, .error = {} });
                    return;
                }
                auto weak = weak_from_this();
                auto weak_job = std::weak_ptr<Job>(job);
                job->watch = pnm::msg::watch(job->execution, [weak]() noexcept {
                    if (auto s = weak.lock())
                        s->notification.signal();
                });
                job->completion.then([weak, weak_job]() noexcept {
                    if (auto j = weak_job.lock())
                        j->ready.store(true);
                    if (auto s = weak.lock())
                        s->notification.signal();
                });
                jobs.push_back(job);
                if (job->group)
                    group_active.store(true);
            } catch (const std::invalid_argument& e) {
                job->execution.reject({ .result = MotorResult::Rejected,
                                        .error = std::string(e.what()).substr(0, messages::MAX_ERROR_TEXT),
                                        .invalid_argument = true });
            } catch (const std::exception& e) {
                job->execution.reject({ .result = MotorResult::Rejected,
                                        .error = std::string(e.what()).substr(0, messages::MAX_ERROR_TEXT) });
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
                        group_active.store(false);
                    messages::Outcome outcome{ .result = *result, .error = {} };
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
                j->execution.feedback({ .execution_id = j->id,
                                        .pulses = snapshot.status.generator.pulses[i],
                                        .position = a.position.get(),
                                        .referenced = a.referenced });
            }
            bus.topic<^^messages::Topics::state>().publish(messages::State(snapshot));
            messages::FaultState faults{};
            auto drivers = controller->driverStatus();
            for (unsigned i = 0; i < 3; ++i)
                faults.drivers[i] = messages::DriverState(drivers[i].status);
            faults.link_inhibited = inhibited.load();
            bus.topic<^^messages::Topics::fault>().publish(faults);
        }
        void run(const std::stop_token& stop) noexcept
        {
            auto next = std::chrono::steady_clock::now();
            while (!stop.stop_requested()) {
                try {
                    if (loss_pending.exchange(false))
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
                job->execution.cancelled({ .result = MotorResult::Stopped, .error = {} });
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
            requests.emplace(bus.service<^^messages::Services::request>()
                               .serveDeferred([weak](const auto& q, Reply reply) {
                if (auto s = weak.lock())
                    s->enqueue({ q, std::move(reply), {}, {}, {} }, false);
            }, { REQUEST_CAPACITY }));
            stops.emplace(bus.service<^^messages::Services::stop>()
                            .serveDeferred([weak](const auto& q, Reply reply) {
                if (auto s = weak.lock())
                    s->enqueue({ q, std::move(reply), {}, {}, {} }, true);
            }, { REQUEST_CAPACITY }));
            actions.emplace(
              bus.action<^^messages::Actions::execute>()
                .serveDeferred([weak](const auto& q, Execution execution) {
                if (auto s = weak.lock())
                    s->enqueue({ { .operation = q.operation, .motor = q.motor }, {}, std::move(execution), {}, q }, false);
            }, { REQUEST_CAPACITY }));
            auto wake = [weak]() noexcept {
                if (auto s = weak.lock())
                    s->dispatch();
            };
            registrations[0] = pnm::msg::watch(*requests, wake);
            registrations[1] = pnm::msg::watch(*stops, wake);
            registrations[2] = pnm::msg::watch(*actions, wake);
            worker = runtime::thread::create_jthread(
              { .name = "IO commands", .priority = IO_WORKER_PRIORITY, .stack_size = IO_WORKER_STACK_SIZE },
              [self = shared_from_this()](const std::stop_token& stop) { self->run(stop); });
        }
        void close()
        {
            closing.store(true);
            if (actions)
                actions->requestStop();
            worker.request_stop();
            notification.signal();
            if (worker.joinable())
                worker.join();
            if (requests)
                requests->close();
            if (stops)
                stops->close();
            if (actions)
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
    void ControllerServer::linkChanged(p2p::ConnectionStatus status) { m_state->link(status); }
}
