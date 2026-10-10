#include "ControllerClient.hpp"
#include <deque>

namespace control
{
    using namespace std::chrono_literals;
    using Op = protocol::Operation;
    namespace
    {
        protocol::Request message(Op operation,
                                  std::optional<ControllerTypes::MotorId> motor = {},
                                  p2p::Bytes data = {})
        {
            return { .operation = operation,
                     .motor = motor ? static_cast<std::uint8_t>(*motor) : std::uint8_t(protocol::ALL_MOTORS),
                     .data = std::move(data) };
        }
        void checked(const protocol::Response& response)
        {
            if (!response.success)
                throw std::runtime_error(response.error);
        }
    }
    struct ControllerClient::State
    {
        pnm::msg::Bus& bus;
        pnm::coro::Context& context;
        pnm::coro::Bus coroutine_bus;
        struct Job : std::enable_shared_from_this<Job>
        {
            explicit Job(pnm::coro::Context& c)
              : context(c)
              , future(completion.getFuture().share())
            {
            }
            pnm::coro::Context& context;
            MotionId id{};
            MotorId motor{};
            bool group{};
            Operation operation{};
            pnm::units::Time duration;
            Completion<MotorResult> completion;
            std::shared_future<MotorResult> future;
            std::promise<void> admission;
            bool admission_resolved{}, finished{};
            std::atomic_bool terminal_confirmed;
            std::atomic_bool scheduled;
            std::optional<pnm::msg::PendingGoal<protocol::Progress, protocol::Outcome>> goal;
            pnm::msg::ReadinessRegistration watch;
            // Clang diagnoses the compiler's implicit static promise-hook calls.
            // NOLINTBEGIN(readability-static-accessed-through-instance)
            static pnm::coro::Task<void> pollOnContext(std::shared_ptr<Job> self)
            {
                self->scheduled.store(false);
                try {
                    if (self->goal)
                        self->goal->poll();
                } catch (...) {
                    self->finish(MotorResult::Faulted, "motion response dispatch failed", false, false);
                }
                co_return;
            }
            // NOLINTEND(readability-static-accessed-through-instance)
            void signal() noexcept
            {
                if (scheduled.exchange(true))
                    return;
                try {
                    pnm::coro::co_spawn(context,
                                        [self = shared_from_this()](pnm::coro::Context& /*context*/) {
                        return pollOnContext(self);
                    });
                } catch (...) {
                    scheduled.store(false);
                }
            }
            void finish(MotorResult result,
                        const std::string& error = {},
                        bool invalid = false,
                        bool confirmed = true)
            {
                if (finished)
                    return;
                finished = true;
                terminal_confirmed.store(confirmed);
                if (!admission_resolved) {
                    admission_resolved = true;
                    const auto reason = error.empty() ? "motion rejected before admission" : error;
                    admission.set_exception(invalid ? std::make_exception_ptr(std::invalid_argument(reason))
                                                    : std::make_exception_ptr(std::runtime_error(reason)));
                }
                completion.setValue(result);
            }
        };
        std::mutex mutex;
        std::deque<std::shared_ptr<Job>> jobs;
        MotionId next{ 1 };
        State(pnm::msg::Bus& b, pnm::coro::Context& c)
          : bus(b)
          , context(c)
          , coroutine_bus(c, b)
        {
        }
    };
    ControllerClient::ControllerClient(pnm::msg::Bus& bus, pnm::coro::Context& context)
      : m_state(std::make_shared<State>(bus, context))
    {
    }
    ControllerClient::~ControllerClient()
    {
        std::scoped_lock lock(m_state->mutex);
        for (auto& job : m_state->jobs)
            if (job->goal)
                job->goal->requestCancel();
    }
    protocol::Response ControllerClient::request(const protocol::Request& q) const
    {
        const auto name = (q.operation == Op::Stop || q.operation == Op::Disable) ? protocol::STOP_NAME
                                                                                  : protocol::REQUEST_NAME;
        auto response = m_state->bus.service<protocol::Request, protocol::Response>(name).call(q, 5s);
        if (!response)
            throw std::runtime_error("IO request failed (unavailable, busy, timeout or disconnected); "
                                     "physical state may be unknown");
        checked(*response);
        return std::move(*response);
    }
    // Coroutine promise hooks are implicitly invoked through their instance.
    // NOLINTBEGIN(readability-static-accessed-through-instance)
    pnm::coro::Task<protocol::Response> ControllerClient::requestAsync(protocol::Request q)
    {
        auto state = m_state;
        auto response = co_await state->coroutine_bus
                          .service<protocol::Request, protocol::Response>(
                            (q.operation == Op::Stop || q.operation == Op::Disable) ? protocol::STOP_NAME
                                                                                    : protocol::REQUEST_NAME)
                          .request(std::move(q), 5s);
        if (!response)
            throw std::runtime_error("IO request failed");
        checked(*response);
        co_return std::move(*response);
    }
    // NOLINTEND(readability-static-accessed-through-instance)
    void ControllerClient::enable() { request(message(Op::Enable)); }
    void ControllerClient::disable() { request(message(Op::Disable)); }
    void ControllerClient::stop(std::optional<MotorId> motor) { request(message(Op::Stop, motor)); }
    void ControllerClient::reset() { request(message(Op::Reset)); }
    void ControllerClient::initializeDrivers() { request(message(Op::InitializeDrivers)); }
    void ControllerClient::configureDriver(MotorId id, DriverConfiguration c)
    {
        p2p::Writer w;
        protocol::write(w, c);
        request(message(Op::ConfigureDriver, id, std::move(w.bytes)));
    }
    void ControllerClient::configureAxis(MotorId id, Conversion c)
    {
        p2p::Writer w;
        protocol::write(w, c);
        request(message(Op::ConfigureAxis, id, std::move(w.bytes)));
    }
    ControllerClient::PlanningSnapshot ControllerClient::planningSnapshot() const
    {
        auto response = request(message(Op::Snapshot));
        p2p::Reader r(response.data);
        auto s = protocol::snapshot(r);
        r.finish();
        return s;
    }
    ControllerClient::Status ControllerClient::status() const { return planningSnapshot().status; }
    std::array<ControllerClient::DriverStatus, 3> ControllerClient::driverStatus() const
    {
        auto response = request(message(Op::Drivers));
        p2p::Reader r(response.data);
        auto s = protocol::drivers(r);
        r.finish();
        return s;
    }
    ControllerClient::AxisConfig ControllerClient::motorConfiguration(MotorId id) const
    {
        return planningSnapshot().configuration.at(static_cast<unsigned>(id));
    }
    ControllerClient::Conversion ControllerClient::axisConversion(MotorId id) const
    {
        auto c = planningSnapshot().status.axes.at(static_cast<unsigned>(id)).conversion;
        if (!c)
            throw std::runtime_error("axis mechanics not configured");
        return *c;
    }
    MotionDefaults ControllerClient::defaults(MotorId id) const
    {
        return planningSnapshot().defaults.at(static_cast<unsigned>(id));
    }
    void ControllerClient::setDefaults(MotorId id, MotionDefaults d)
    {
        p2p::Writer w;
        protocol::write(w, d);
        request(message(Op::SetDefaults, id, std::move(w.bytes)));
    }
    ControllerClient::AxisDefaults ControllerClient::axisDefaults(MotorId id) const
    {
        auto response = request(message(Op::AxisDefaults, id));
        p2p::Reader r(response.data);
        auto d = protocol::axis_defaults(r);
        r.finish();
        return d;
    }
    void ControllerClient::setAxisDefaults(MotorId id, const AxisDefaults& d)
    {
        p2p::Writer w;
        protocol::write(w, d);
        request(message(Op::SetAxisDefaults, id, std::move(w.bytes)));
    }
    hal::step::PulseCount ControllerClient::setVelocity(MotorId id, pnm::units::AngularVelocity speed)
    {
        p2p::Writer w;
        w.real(speed.get());
        auto response = request(message(Op::Velocity, id, std::move(w.bytes)));
        p2p::Reader r(response.data);
        auto n = r.integer<std::uint64_t>();
        r.finish();
        return n;
    }
    hal::step::PulseCount ControllerClient::setAxisVelocity(MotorId id, const AxisSpeed& speed)
    {
        p2p::Writer w;
        protocol::write(w, speed);
        auto response = request(message(Op::AxisVelocity, id, std::move(w.bytes)));
        p2p::Reader r(response.data);
        auto n = r.integer<std::uint64_t>();
        r.finish();
        return n;
    }
    ControllerClient::Motion ControllerClient::submit(const protocol::Request& q,
                                                      bool group,
                                                      pnm::units::Time duration)
    {
        auto state = m_state;
        auto job = std::make_shared<State::Job>(state->context);
        auto accepted = job->admission.get_future();
        job->motor = q.motor == protocol::ALL_MOTORS ? MotorId::Motor1 : static_cast<MotorId>(q.motor);
        job->group = group;
        job->duration = duration;
        job->operation = q.operation == Op::ReferenceAll ? Operation::Reference : Operation::Move;
        {
            std::scoped_lock lock(state->mutex);
            if (std::cmp_equal(
                  std::ranges::count_if(state->jobs, [&](const auto& j) { return j->group == group; }),
                  MOTION_HISTORY_CAPACITY)) {
                auto old = std::ranges::find_if(state->jobs, [&](const auto& j) {
                    return j->group == group && j->completion.result().has_value();
                });
                if (old == state->jobs.end())
                    throw std::runtime_error("motion job registry full");
                state->jobs.erase(old);
            }
            state->jobs.push_back(job);
        }
        auto weak = std::weak_ptr<State::Job>(job);
        try {
            job->goal.emplace(
              state->bus
                .action<protocol::Request, protocol::Progress, protocol::Outcome>(protocol::EXECUTE_NAME)
                .sendGoal(q, { 2s }, { .on_accepted = [weak] {
                if (auto j = weak.lock()) {
                    if (!j->admission_resolved) {
                        j->admission_resolved = true;
                        j->admission.set_value();
                    }
                }
            }, .on_feedback = {}, .on_result = [weak](auto result) {
                if (auto j = weak.lock()) {
                    if (result)
                        j->finish(result->value.result, result->value.error, result->value.invalid_argument);
                    else
                        j->finish(MotorResult::Faulted,
                                  "IO motion failed or link lost; stopped state is not confirmed",
                                  false,
                                  false);
                }
            } }));
            job->watch = pnm::msg::watch(*job->goal, [weak]() noexcept {
                if (auto j = weak.lock())
                    j->signal();
            });
            if (accepted.wait_for(3s) != std::future_status::ready) {
                job->goal->requestCancel();
                throw std::runtime_error("IO motion admission timed out");
            }
            accepted.get();
            {
                std::scoped_lock lock(state->mutex);
                job->id = state->next++;
            }
        } catch (...) {
            if (job->goal)
                job->goal->requestCancel();
            {
                std::scoped_lock lock(state->mutex);
                std::erase(state->jobs, job);
            }
            throw;
        }
        return { .id = job->id,
                 .motor = job->motor,
                 .result = job->completion.result(),
                 .completion = job->future,
                 .observed = job->completion,
                 .terminal_confirmed = job->terminal_confirmed.load() };
    }
    ControllerClient::Motion ControllerClient::move(MotorId id, const Move& m)
    {
        p2p::Writer w;
        protocol::write(w, m);
        return submit(message(Op::Move, id, std::move(w.bytes)), false);
    }
    ControllerClient::Motion ControllerClient::moveAxis(MotorId id, const AxisMove& m)
    {
        p2p::Writer w;
        protocol::write(w, m);
        return submit(message(Op::MoveAxis, id, std::move(w.bytes)), false);
    }
    ControllerClient::Motion ControllerClient::reference(MotorId id,
                                                         pnm::units::AngularVelocity seek,
                                                         pnm::units::AngularVelocity latch,
                                                         pnm::units::Time timeout)
    {
        p2p::Writer w;
        w.real(seek.get());
        w.real(latch.get());
        w.real(timeout.get());
        return submit(message(Op::Reference, id, std::move(w.bytes)), false);
    }
    ControllerClient::Motion ControllerClient::referenceAxis(MotorId id,
                                                             std::optional<AxisSpeed> seek,
                                                             std::optional<AxisSpeed> latch,
                                                             pnm::units::Time timeout)
    {
        p2p::Writer w;
        w.boolean(seek.has_value());
        if (seek)
            protocol::write(w, *seek);
        w.boolean(latch.has_value());
        if (latch)
            protocol::write(w, *latch);
        w.real(timeout.get());
        return submit(message(Op::ReferenceAxis, id, std::move(w.bytes)), false);
    }
    ControllerClient::GroupMotion ControllerClient::referenceAll(pnm::units::Time timeout)
    {
        p2p::Writer w;
        w.real(timeout.get());
        auto m = submit(message(Op::ReferenceAll, {}, std::move(w.bytes)), true);
        return { .id = m.id,
                 .operation = Operation::Reference,
                 .duration = {},
                 .result = m.result,
                 .completion = m.completion,
                 .observed = m.observed,
                 .terminal_confirmed = m.terminal_confirmed };
    }
    ControllerClient::GroupMotion ControllerClient::coordinate(const Trajectory& t, pnm::units::Time timeout)
    {
        p2p::Writer w;
        protocol::write(w, t);
        w.real(timeout.get());
        auto m = submit(message(Op::Trajectory, {}, std::move(w.bytes)), true, t.duration);
        return { .id = m.id,
                 .operation = Operation::Move,
                 .duration = t.duration,
                 .result = m.result,
                 .completion = m.completion,
                 .observed = m.observed,
                 .terminal_confirmed = m.terminal_confirmed };
    }
    std::vector<ControllerClient::Motion> ControllerClient::motions()
    {
        std::scoped_lock lock(m_state->mutex);
        std::vector<Motion> result;
        for (auto& j : m_state->jobs)
            if (j->id && !j->group)
                result.push_back({ j->id,
                                   j->motor,
                                   j->completion.result(),
                                   j->future,
                                   j->completion,
                                   j->terminal_confirmed.load() });
        return result;
    }
    std::vector<ControllerClient::GroupMotion> ControllerClient::groupMotions()
    {
        std::scoped_lock lock(m_state->mutex);
        std::vector<GroupMotion> result;
        for (auto& j : m_state->jobs)
            if (j->id && j->group)
                result.push_back({ j->id,
                                   j->operation,
                                   j->duration,
                                   j->completion.result(),
                                   j->future,
                                   j->completion,
                                   j->terminal_confirmed.load() });
        return result;
    }
    void ControllerClient::cancel(MotionId id)
    {
        std::scoped_lock lock(m_state->mutex);
        for (auto& j : m_state->jobs)
            if (j->id == id) {
                if (j->goal)
                    j->goal->requestCancel();
                return;
            }
        throw std::invalid_argument("unknown motion ID");
    }
}
