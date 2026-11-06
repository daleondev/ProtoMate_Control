#include "Controller.hpp"
#include <pneumo/logging.hpp>
#include <algorithm>
#include <cmath>

extern "C" unsigned motion_debug_fault_line;
extern "C" unsigned motion_debug_executor_abort;
namespace io::motion
{
    namespace tr = ::motion::trajectory;
    using F = ::motion::Family;
    using J = ::motion::JobState;

    void Controller::connectionLost()
    {
        std::scoped_lock lock{m_mutex};
        m_admissionLost=true;++m_controlEpoch;
        m_ownership.expire(Ownership::Clock::now());
        m_executionNotification.signal();
    }
    Ownership::Admission Controller::reserveMotor(std::uint64_t job, MotorId id)
    {
        std::scoped_lock lock{m_mutex};
        if (static_cast<unsigned>(id) >= m_motors.size()) return {tr::Error::InvalidInput};
        const auto mask = std::uint8_t(1U << static_cast<unsigned>(id));
        auto result = m_ownership.acquire(job, F::Motor, mask, 0, Ownership::Clock::now());
        if (result.error == tr::Error::None) {
            m_ownership.find(job)->state = J::Running;
            m_executionNotification.signal();
        }
        return result;
    }
    void Controller::releaseJob(std::uint64_t job)
    {
        std::scoped_lock lock{m_mutex};
        for (auto& g : m_groups) if (g.managed && g.job == job) {
            if ((g.executor && g.executor->reserved()) || (g.homing && g.started && !g.home_finished)) return;
            g.managed = false;
            g.completed = {};
        }
        m_ownership.release(job);
    }
    void Controller::stopProgramsLocked(std::uint8_t mask, bool abort)
    {
        for (auto& g : m_groups) {
            if (!(g.mask & mask)) continue;
            if (g.managed && !g.started) {
                if (g.executor && g.executor->reserved()) g.executor->stop(g.executor->status().id);
                m_ownership.release(g.job);
                g.managed=false;g.completed={};g.homing=false;
                continue;
            }
            if (g.homing && g.started && !g.home_finished) {
                g.error = abort ? tr::Error::Fault : tr::Error::Cancelled;
                if (auto* owner=m_ownership.find(g.job)) owner->state=J::Stopping;
                for (unsigned i=0;i<3;++i) if (g.mask & (1U<<i)) {
                    m_motors[i]->requestStop();
                    if(abort)m_motors[i]->invalidateReference();
                }
                continue;
            }
            if (!g.executor || !g.executor->reserved()) continue;
            if (auto* owner = m_ownership.find(g.job)) owner->state = J::Stopping;
            if (abort) {
                g.executor->abort();
                for (unsigned i=0; i<3; ++i)
                    if (g.mask & (1U<<i)) m_motors[i]->invalidateReference();
            } else g.executor->stop(g.executor->status().id);
        }
        m_executionNotification.signal();
    }
    tr::Reply Controller::control(const tr::Request& request)
    {
        std::scoped_lock lock{m_mutex};
        if(request.operation==tr::Operation::Renew){
            tr::Reply ack;ack.acknowledgement=true;ack.boot=m_boot;
            if(request.version!=tr::VERSION)ack.error=tr::Error::Protocol;
            else if(request.boot!=m_boot)ack.error=tr::Error::Stale;
            else {
                const auto now=Ownership::Clock::now();
                for(auto job:request.renew_jobs)if(job)m_ownership.renew(job,now);
            }
            return ack;
        }
        auto result = trajectorySnapshotLocked();
        const auto fail = [&](tr::Error error) { result.error = error; return result; };
        if (request.version != tr::VERSION) return fail(tr::Error::Protocol);
        if (request.operation == tr::Operation::Inspect) return result;
        if (request.boot != m_boot) return fail(tr::Error::Stale);
        const auto now = Ownership::Clock::now();
        if (request.operation == tr::Operation::Acquire) {
            if (m_admissionLost || !m_initialized || m_enabling || !enabled() || !driversReady()) return fail(tr::Error::Fault);
            if (request.family != F::Axis && request.family != F::Robot) return fail(tr::Error::InvalidInput);
            if (!request.configuration) return fail(tr::Error::Configuration);
            // Also reject legacy/direct submissions that do not carry a UI job.
            for (const auto& motor : m_motors) {
                std::scoped_lock queue{motor->m_queueMutex};
                if (m_ownership.family()==F::None && (motor->m_busy || motor->m_homing || motor->m_pending))
                    return fail(tr::Error::Busy);
            }
            const auto admission = m_ownership.acquire(request.job, request.family, request.axes,
                                                       request.configuration, now);
            result.error = admission.error;
            result.session = admission.token;
            result.job = admission.busy_job ? admission.busy_job : request.job;
            result.family = m_ownership.family(); result.owners = m_ownership.owners();
            if (admission.error == tr::Error::None) m_executionNotification.signal();
            return result;
        }
        // Global stop/abort and per-axis stop bypass family ownership. A target
        // within a coordinated group always stops the entire group.
        if (request.operation == tr::Operation::Stop || request.operation == tr::Operation::Abort) {
            std::uint8_t mask = request.axes ? request.axes : 7;
            if (request.job) {
                const auto* owner = m_ownership.find(request.job);
                if (!owner) return fail(tr::Error::Stale);
                mask = owner->axes;
            }
            const bool abort = request.operation == tr::Operation::Abort;
            stopProgramsLocked(mask, abort);
            for (const auto& e : m_ownership.entries()) if (e.job && (e.axes & mask)) {
                m_ownership.find(e.job)->state = J::Stopping;
                if (e.family == F::Motor)
                    for (unsigned i=0; i<3; ++i) if (e.axes & (1U<<i)) {
                        m_motors[i]->requestStop();
                        if (abort) m_motors[i]->invalidateReference();
                    }
            }
            // A cancelled plan has no hardware/worker cleanup to retain.
            for (const auto& e:m_ownership.entries())
                if(e.job && e.state==J::Stopping && e.family!=F::Motor && (e.axes&mask) &&
                   std::ranges::none_of(m_groups,[&](const auto& g){return g.managed&&g.job==e.job;}))
                    m_ownership.release(e.job);
            m_executionNotification.signal();
            return result;
        }
        auto* owner = m_ownership.find(request.job);
        if (!owner || owner->token != request.session) return fail(tr::Error::Stale);
        if (request.operation == tr::Operation::Release) {
            for (auto& g : m_groups) if (g.managed && g.job == request.job) {
                if ((g.executor && g.executor->active()) || (g.homing && g.started && !g.home_finished)) return fail(tr::Error::Busy);
                if (g.executor && g.executor->reserved()) g.executor->stop(g.executor->status().id);
                g.managed = false; g.completed = {};
            }
            m_ownership.release(request.job);
            return result;
        }
        return fail(tr::Error::Unsupported);
    }
    tr::Reply Controller::prepareProgram(const tr::Request& request, std::stop_token stop)
    {
        std::unique_lock lock{m_mutex};
        auto result = trajectorySnapshotLocked();
        result.job = request.job; result.session = request.session;
        const auto fail = [&](tr::Error error) { result.error = error; return result; };
        auto* owner = m_ownership.find(request.job);
        if (request.operation != tr::Operation::Prepare || request.boot != m_boot || !owner ||
            owner->token != request.session || owner->state != J::Planning ||
            owner->deadline <= Ownership::Clock::now()) return fail(tr::Error::Stale);
        if (stop.stop_requested()) return fail(tr::Error::Cancelled);
        if (request.configuration != owner->configuration) return fail(tr::Error::Configuration);
        for (unsigned i=0; i<3; ++i) if (owner->axes & (1U<<i)) {
            if (request.axis_revisions[i] != m_ownership.revisions()[i] ||
                request.expected_position[i] != result.position[i]) return fail(tr::Error::Stale);
        }
        if (request.program.kind != tr::ProgramKind::Home &&
            (result.referenced & owner->axes) != owner->axes) return fail(tr::Error::Unreferenced);
        if (!result.enabled || !result.counts_exact || !m_initialized || m_enabling || !driversReady())
            return fail(tr::Error::Fault);
        const auto& program = request.program;
        if (!std::isfinite(program.timeout) || program.timeout < 0 || program.timeout > (program.kind==tr::ProgramKind::Jog?1e6:90))
            return fail(tr::Error::InvalidInput);
        if (program.kind == tr::ProgramKind::Home) {
            auto order=program.home_order;std::ranges::sort(order);
            if(order!=std::array<std::uint8_t,3>{0,1,2})return fail(tr::Error::InvalidInput);
            for(unsigned i=0;i<3;++i)if(owner->axes&(1U<<i)){
                const auto& h=program.home[i];
                if(!std::isfinite(h.seek_rpm)||!std::isfinite(h.latch_rpm)||!std::isfinite(h.timeout)||
                   h.seek_rpm<=0||h.latch_rpm<=0||h.latch_rpm>=h.seek_rpm||h.timeout<=0||h.timeout>90||
                   !velocityAllowed(MotorId(i),h.seek_rpm*1_rpm))return fail(tr::Error::InvalidInput);
            }
            auto slot=std::ranges::find_if(m_groups,[](const auto& g){
                return !g.managed&&(!g.executor||!g.executor->reserved());});
            if(slot==m_groups.end())return fail(tr::Error::Capacity);
            slot->job=request.job;slot->mask=owner->axes;slot->managed=true;
            slot->homing=true;slot->home_finished=false;slot->home_stage=0;slot->home_ready=false;
            slot->home=program.home;slot->home_order=program.home_order;
            slot->started=slot->notified=slot->zero=false;slot->duration=0;slot->completed={};slot->error=tr::Error::None;
            owner->state=J::Preparing;result.state=tr::State::Staged;return result;
        }
        if(program.kind!=tr::ProgramKind::Finite && program.kind!=tr::ProgramKind::Jog)
            return fail(tr::Error::Unsupported);
        if(program.kind==tr::ProgramKind::Jog && owner->family!=F::Axis)return fail(tr::Error::InvalidInput);
        bool moving{};
        Executor::Command command{};
        command.rolling=program.kind==tr::ProgramKind::Jog;
        command.id = owner->token; // Monotonic IO identity, independent of UI restarts.
        for (unsigned i=0; i<3; ++i) {
            const auto d = program.displacement[i];
            if (!(owner->axes & (1U<<i))) {
                if (d) return fail(tr::Error::InvalidInput);
                continue;
            }
            const auto& l = program.limits[i];
            const auto& hw = result.capabilities.limits[i];
            for (auto v : {l.minimum,l.maximum,l.velocity,l.acceleration,l.deceleration,l.jerk})
                if (!std::isfinite(v)) return fail(tr::Error::InvalidInput);
            if (l.minimum > l.maximum || l.minimum < hw.minimum || l.maximum > hw.maximum ||
                l.velocity <= 0 || l.velocity > hw.velocity || l.acceleration <= 0 ||
                l.acceleration > hw.acceleration || l.deceleration <= 0 || l.deceleration > hw.deceleration ||
                l.jerk <= 0 || l.jerk > hw.jerk) return fail(tr::Error::Configuration);
            if (std::abs(double(d)) > tr::MAX_POSITION ||
                result.position[i] < l.minimum || result.position[i] > l.maximum ||
                double(result.position[i]) + double(d) < l.minimum ||
                double(result.position[i]) + double(d) > l.maximum) return fail(tr::Error::PositionLimit);
            if (!d) continue;
            moving = true;
            command.axes[i].limits = {l.velocity,l.acceleration,l.deceleration,l.jerk};
            command.axes[i].forward = d > 0;
        }
        // The reservation protects the selected axes while numerical work runs.
        // Other groups, urgent controls and ownership renewals retain the lock.
        const auto epoch = m_controlEpoch;
        lock.unlock();
        std::unique_ptr<Executor> compiled;
        if (moving) {
            const auto law = ::motion::Profile::fromDescription(program.progress);
            if (!law || law->distance() != 1. || law->duration() > (command.rolling?1e6 : ::motion::operating::MAX_PROGRAM_SECONDS))
                return fail(tr::Error::Timing);
            command.progress = program.progress;
            command.progress_limits = program.progress_limits;
            for (unsigned i=0; i<3; ++i) if (program.displacement[i]) {
                const auto p = law->scaled(std::abs(double(program.displacement[i])));
                if (!p) return fail(tr::Error::InvalidInput);
                command.axes[i].profile = p->description();
            }
            compiled = std::make_unique<Executor>(m_generator,
              [this](unsigned i, bool forward, std::shared_ptr<const hal::step::Sequence> sequence) {
                  return m_motors[i]->prepareCoordinated(forward, std::move(sequence));
              }, m_profileMemory);
            if (!compiled->prepare(command, false)) return fail(tr::Error::NoStoppingPath);
        }
        lock.lock();
        owner = m_ownership.find(request.job);
        if (stop.stop_requested()) return fail(tr::Error::Cancelled);
        if (!owner || owner->token != request.session || owner->state != J::Planning ||
            epoch != m_controlEpoch || owner->deadline <= Ownership::Clock::now()) return fail(tr::Error::Stale);
        if (m_admissionLost || !m_initialized || m_enabling || !enabled() || !driversReady()) return fail(tr::Error::Fault);
        for (unsigned i=0; i<3; ++i) if (owner->axes & (1U<<i)) {
            if (request.axis_revisions[i] != m_ownership.revisions()[i]) return fail(tr::Error::Stale);
            if (!m_motors[i]->isReferenced() ||
                (program.displacement[i]>0 && m_motors[i]->referenceSwitchActive())) return fail(tr::Error::Fault);
        }
        auto slot = std::ranges::find_if(m_groups, [](const auto& g) {
            return !g.managed && (!g.executor || !g.executor->reserved());
        });
        if (slot == m_groups.end()) return fail(tr::Error::Capacity);
        if (compiled) {
            for (unsigned i=0; i<3; ++i) if(program.displacement[i]) m_motors[i]->stopAndWait();
            for (auto& motor : m_motors) motor->m_events->group_notification.store(&m_executionNotification);
            if (!compiled->bind()) return fail(tr::Error::Fault);
            slot->executor = std::move(compiled);
            slot->moving = 0;
            for(unsigned i=0;i<3;++i)if(program.displacement[i])slot->moving |= 1U<<i;
        }
        slot->managed = true; slot->job = request.job; slot->mask = owner->axes;
        slot->homing=false;
        if(!moving)slot->moving=0;
        slot->zero = !moving; slot->started = slot->notified = false; slot->completed = {};
        slot->error = tr::Error::None;
        slot->duration=program.timeout;
        slot->timeout=Ownership::Clock::time_point::max();
        owner->state = J::Preparing;
        result.state = tr::State::Staged;
        return result;
    }
    tr::Error Controller::startProgram(const tr::Run& run,
      std::function<void(tr::Update)> completed)
    {
        std::scoped_lock lock{m_mutex};
        auto* owner = m_ownership.find(run.job);
        auto slot = std::ranges::find_if(m_groups,[&](const auto& g) {return g.managed && g.job==run.job;});
        if (run.boot != m_boot || !owner || owner->token != run.session || owner->state != J::Preparing ||
            Ownership::Clock::now() >= owner->deadline || slot == m_groups.end() || !slot->managed)
            return tr::Error::Stale;
        if (m_admissionLost || !m_initialized || m_enabling || !enabled() || !driversReady()) return tr::Error::Fault;
        for (unsigned i=0; i<3; ++i) if (slot->mask & (1U<<i)) {
            if (m_ownership.revisions()[i] != owner->revisions[i]) return tr::Error::Stale;
            if (!slot->homing && (!m_motors[i]->isReferenced() || ((slot->moving&(1U<<i))&&m_motors[i]->coordinatedBlocked()))) return tr::Error::Fault;
        }
        if (!slot->zero && !slot->homing) {
            const auto start = slot->executor->start(owner->token);
            if (!start) return tr::Error::Fault;
        }
        slot->timeout=slot->duration>0 ? Ownership::Clock::now()+
          std::chrono::duration_cast<Ownership::Clock::duration>(std::chrono::duration<double>(slot->duration)):
          Ownership::Clock::time_point::max();
        slot->started = true; slot->completed = std::move(completed);
        owner->state = J::Running;
        m_executionNotification.signal();
        return tr::Error::None;
    }
    void Controller::finishProgramsLocked()
    {
        for (auto& g : m_groups) {
            if (!g.managed || !g.started || g.notified ||
                (g.homing ? !g.home_finished : (!g.zero && g.executor->reserved()))) continue;
            tr::Update update{.job=g.job, .state=J::Completed, .error=g.error};
            if (g.homing) {
                if(g.error==tr::Error::Fault)update.state=J::Faulted;
                else if(g.error!=tr::Error::None)update.state=J::Stopped;
            } else if (!g.zero) {
                const auto status = g.executor->status();
                update.counts_exact = status.counts_exact;
                if (status.state == Executor::State::Faulted) { update.state=J::Faulted; update.error=tr::Error::Fault; }
                else if (status.state == Executor::State::Stopped) {
                    update.state=J::Stopped;
                    if (status.reason==Executor::Reason::RenewalLate) update.error=tr::Error::Timing;
                    else if (status.reason==Executor::Reason::LeaseExpired) update.error=tr::Error::LeaseExpired;
                }
                pnm::log::debug("Execution job {} ended: state={} reason={} pulses={},{},{} elapsed={} frontier={} renewals={} rejected={} error={} hal_site={} exec_site={}",
                  g.job, unsigned(status.state), unsigned(status.reason), status.pulses[0],status.pulses[1],status.pulses[2],
                  status.elapsed,status.stopping_at,status.renewals,status.rejected_renewals,status.last_error,motion_debug_fault_line,motion_debug_executor_abort);
            }
            for (unsigned i=0; i<3; ++i) {
                const auto step = m_configuration[i].full_step_angle.get() / m_configuration[i].microsteps;
                const auto p = tr::quantize(m_motors[i]->setpointPosition().get() / step);
                if (p) update.position[i] = *p; else update.counts_exact = false;
            }
            g.notified = true;
            if (auto* owner = m_ownership.find(g.job)) owner->state = update.state;
            // Callback only copies the bounded result and notifies Server. It
            // must not enter Controller or perform messaging while under lock.
            if (g.completed) g.completed(update);
        }
    }
}
