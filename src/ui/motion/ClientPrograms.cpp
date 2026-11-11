#include "Client.hpp"
#include "common/motion/OperatingEnvelope.hpp"
#include <algorithm>
#include <format>
#include <pneumo/logging.hpp>
#include <stdexcept>
#include <utility>

namespace ui::motion
{
    namespace tr = ::motion::trajectory;
    using namespace std::chrono_literals;
    std::shared_ptr<Client::Job> Client::findLocked(std::uint64_t id) const
    {
        const auto it{ std::ranges::find_if(m_jobs, [&](const auto& j) { return j->info.id == id; }) };
        if (it == m_jobs.end()) {
            throw std::invalid_argument("unknown motion job");
        }
        return *it;
    }
    std::shared_ptr<Client::Job> Client::createLocked(::motion::Family family, std::uint8_t axes, std::uint64_t source)
    {
        if (source && !remoteAuthorizedLocked(source)) throw std::runtime_error("Remote control expired or permission revoked");
        if (family == ::motion::Family::Motor) checkRawOwnerLocked(axes, source);
        if (m_stopped || !m_connected) {
            throw std::runtime_error("IO controller unavailable");
        }
        if (!axes || axes > ::motion::ALL_AXES_MASK || family == ::motion::Family::None ||
            family > ::motion::Family::Robot) {
            throw std::invalid_argument("invalid motion selection");
        }
        if (!m_nextId) {
            throw std::overflow_error("motion job identifiers exhausted");
        }
        retireJobsLocked();
        if (m_jobs.size() >= MAX_JOBS) {
            throw std::runtime_error("motion admission capacity is full; stop or wait for a job");
        }
        if (m_jobs.size() + m_history->count >= HISTORY_CAPACITY) {
            std::move(m_history->jobs.begin() + 1,
                      m_history->jobs.begin() + m_history->count,
                      m_history->jobs.begin());
            --m_history->count;
        }
        auto job{ std::make_shared<Job>() };
        job->info.id = m_nextId++;
        job->info.source = source;
        job->info.family = family;
        job->info.axes = axes;
        job->preparing = family != ::motion::Family::Motor;
        m_jobs.push_back(job);
        return job;
    }
    std::uint64_t Client::create(::motion::Family family, std::uint8_t axes, std::uint64_t source, bool held)
    {
        std::scoped_lock lock{ m_mutex };
        auto job{ createLocked(family, axes, source) };
        if (held && source) job->inputDeadline = std::chrono::steady_clock::now() + REMOTE_TIMEOUT;
        return job->info.id;
    }
    std::stop_token Client::cancellation(std::uint64_t id) const
    {
        std::scoped_lock lock{ m_mutex };
        return findLocked(id)->stop.get_token();
    }
    tr::Reply Client::exchange(const tr::Request& value, bool urgent, const std::stop_token& stop)
    {
        auto call{ (urgent ? m_control : m_trajectory).request(tr::encode(value), REQUEST_TIMEOUT, stop) };
        const auto encoded{ call.get() };
        if (!encoded) {
            throw std::runtime_error(
              std::format("motion operation {} failed (service error {}); hardware stop unconfirmed",
                          unsigned(value.operation),
                          unsigned(encoded.error())));
        }
        const auto decoded{ tr::decode_reply(*encoded) };
        if (!decoded) {
            throw std::runtime_error("invalid motion response");
        }
        if (decoded->generation) {
            std::scoped_lock lock{ m_mutex };
            m_commandGeneration = std::max(m_commandGeneration.value_or(0), decoded->generation);
        }
        return *decoded;
    }
    tr::Reply Client::inspect(const std::stop_token& stop)
    {
        auto result{ exchange({}, false, stop) };
        std::scoped_lock lock{ m_mutex };
        m_boot = result.boot;
        return result;
    }
    tr::Reply Client::acquire(Reservation reservation, const std::stop_token& stop)
    {
        const auto [id, configuration, axes]{ reservation };
        std::uint64_t boot{ 0 };
        {
            std::scoped_lock lock{ m_mutex };
            boot = m_boot;
        }
        // The planner has already inspected the model. Acquire returns the
        // authoritative reservation snapshot; a second Inspect adds no safety.
        // A restarted peer must reject an old boot ID, never replay the command.
        if (!boot) {
            boot = inspect(stop).boot;
        }
        tr::Request request{ .operation = tr::Operation::Acquire,
                             .boot = boot,
                             .configuration = configuration };
        {
            std::scoped_lock lock{ m_mutex };
            auto job{ findLocked(id) };
            if (job->stop.stop_requested()) {
                throw std::runtime_error("motion preparation cancelled");
            }
            if (axes) {
                job->info.axes = axes;
            }
            request.family = job->info.family;
            request.axes = job->info.axes;
            request.job = id;
            job->info.state = JobState::Planning;
        }
        auto result{ exchange(request, false, stop) };
        if (result.error != tr::Error::None) {
            reject(id, result.error, result.job);
            return result;
        }
        {
            std::scoped_lock lock{ m_mutex };
            auto job{ findLocked(id) };
            job->boot = result.boot;
            job->reservation = result.session;
        }
        m_notification->signal();
        return result;
    }
    void Client::releaseReservation(const std::shared_ptr<Job>& job)
    {
        tr::Request request{};
        {
            std::scoped_lock lock{ m_mutex };
            request.operation = tr::Operation::Release;
            request.boot = job->boot;
            request.session = job->reservation;
            request.job = job->info.id;
        }
        if (request.session) {
            try {
                exchange(request, true);
            } catch (...) { // NOLINT(bugprone-empty-catch): The IO lease remains authoritative if reservation
                            // release cannot be delivered.
                            /* IO lease remains authoritative. */
            }
        }
        std::scoped_lock lock{ m_mutex };
        job->reservation = 0;
    }
    void Client::reject(std::uint64_t id, tr::Error error, std::uint64_t busy)
    {
        std::shared_ptr<Job> job{};
        {
            std::scoped_lock lock{ m_mutex };
            job = findLocked(id);
            job->info.error = error;
            job->info.busy_job = error == tr::Error::Busy ? busy : 0;
            switch (error) {
                case tr::Error::Unknown:
                    job->info.state = JobState::Unknown;
                    break;
                case tr::Error::Cancelled:
                    job->info.state = JobState::Stopped;
                    break;
                default:
                    job->info.state = JobState::Rejected;
                    break;
            }
            m_jobChanged.notify_all();
        }
        releaseReservation(job);
    }
    void Client::execute(std::uint64_t id,
                         const tr::Reply& reservation,
                         std::uint64_t configuration,
                         const tr::FiniteProgram& program,
                         const std::stop_token& stop)
    {
        tr::Request request{ .operation = tr::Operation::Prepare,
                             .boot = reservation.boot,
                             .session = reservation.session,
                             .configuration = configuration,
                             .expected_position = reservation.position };
        request.job = id;
        request.axis_revisions = reservation.axis_revisions;
        request.program = program;
        {
            std::scoped_lock lock{ m_mutex };
            findLocked(id)->info.state = JobState::Preparing;
        }
        const auto reply{ exchange(request, false, stop) };
        if (reply.error != tr::Error::None) {
            reject(id, reply.error, reply.error == tr::Error::Busy ? reply.job : 0);
            return;
        }
        std::scoped_lock lock{ m_mutex };
        const auto job{ findLocked(id) };
        if (job->stop.stop_requested()) {
            // The planning worker catches cancellation and releases its reservation.
            throw std::runtime_error("motion preparation cancelled");
        }
        const std::weak_ptr<Job> weak{ job };
        for (unsigned i{ 0 }; i < 3; ++i) {
            job->info.endpoint[i] = program.kind == tr::ProgramKind::Home && program.home_to_neutral
                                      ? program.home_neutral[i]
                                      : reservation.position[i] + program.displacement[i];
        }
        job->program.emplace(m_programs.sendGoal({ reservation.boot, reservation.session, id },
                                                 { .accept_timeout = REQUEST_TIMEOUT },
                                                 { .on_accepted = [weak] {
            if (auto j{ weak.lock() }; j && j->info.state == JobState::Preparing) {
                j->info.state = JobState::Running;
            }
        }, .on_feedback = [weak](const tr::Update& update) {
            if (auto j{ weak.lock() };
                j && !terminal(j->info.state) && update.state == ::motion::JobState::Stopping) {
                j->info.state = JobState::Stopping;
            }
        }, .on_result = [weak](pnm::msg::ActionResult<tr::Update> result) {
            if (auto j{ weak.lock() }) {
                if (!result) {
                    j->info.state = JobState::Unknown;
                    j->info.error = tr::Error::Unknown;
                    return;
                }
                j->info.error = result->value.error;
                switch (result->value.state) {
                    case ::motion::JobState::Completed:
                        j->info.state = JobState::Completed;
                        break;
                    case ::motion::JobState::Stopped:
                        j->info.state = JobState::Stopped;
                        break;
                    case ::motion::JobState::Rejected:
                        j->info.state = JobState::Rejected;
                        break;
                    case ::motion::JobState::Faulted:
                        j->info.state = JobState::Faulted;
                        break;
                    default:
                        j->info.state = JobState::Unknown;
                        break;
                }
            }
        } }));
        job->readiness = pnm::msg::watch(*job->program, [n = m_notification] { n->signal(); });
        m_notification->signal();
    }
    void Client::cancel(std::uint64_t id)
    {
        std::optional<std::stop_source> source{};
        tr::Request request{};
        {
            std::scoped_lock lock{ m_mutex };
            for (unsigned i{ 0 }; i < m_history->count; ++i) {
                if (m_history->jobs[i].id == id) {
                    return;
                }
            }
            auto job{ findLocked(id) };
            if (terminal(job->info.state)) {
                return;
            }
            source.emplace(job->stop);
            if (job->handle) {
                job->handle->requestCancel();
            }
            if (job->program) {
                job->program->requestCancel();
            }
            job->info.state = JobState::Stopping;
            request.operation = tr::Operation::Stop;
            request.boot = job->boot;
            request.session = job->reservation;
            request.job = id;
        }
        source->request_stop();
        if (request.session) {
            exchange(request, true);
        }
        m_notification->signal();
    }
    void Client::cancelFamily(::motion::Family family)
    {
        std::array<std::uint64_t, MAX_JOBS> jobs{};
        unsigned count{};
        {
            std::scoped_lock lock{ m_mutex };
            for (const auto& job : m_jobs) {
                if (job->info.family == family && !terminal(job->info.state)) {
                    jobs[count++] = job->info.id;
                }
            }
        }
        for (unsigned i{ 0 }; i < count; ++i) {
            cancel(jobs[i]);
        }
    }
    void Client::stop(std::uint8_t axes, bool abort)
    {
        std::uint64_t boot{ 0 };
        std::array<std::optional<std::stop_source>, MAX_JOBS> cancel{};
        unsigned n{};
        {
            std::scoped_lock lock{ m_mutex };
            boot = m_boot;
            for (const auto& job : m_jobs) {
                if (!terminal(job->info.state) && (job->info.axes & axes)) {
                    job->info.state = JobState::Stopping;
                    if (n < cancel.size()) {
                        cancel[n++] = job->stop;
                    }
                }
            }
        }
        for (auto& source : cancel) {
            if (source) {
                source->request_stop();
            }
        }
        if (!boot) {
            boot = inspect().boot;
        }
        tr::Request request{ .operation = abort ? tr::Operation::Abort : tr::Operation::Stop, .boot = boot };
        request.axes = axes;
        const auto result{ exchange(request, true) };
        if (result.error != tr::Error::None) {
            throw std::runtime_error(std::string(tr::name(result.error)));
        }
    }
    Client::JobInfo Client::wait(std::uint64_t id,
                                 std::chrono::milliseconds timeout,
                                 const std::stop_token& stop)
    {
        // Serialize cancellation with the predicate check and wait registration.
        const std::stop_callback wake{ stop, [this] {
            const std::scoped_lock lock{ m_mutex };
            m_jobChanged.notify_all();
        } };
        std::unique_lock lock{ m_mutex };
        for (unsigned i{ 0 }; i < m_history->count; ++i) {
            if (m_history->jobs[i].id == id) {
                return m_history->jobs[i];
            }
        }
        auto job{ findLocked(id) };
        const auto deadline{ timeout.count() > 0 ? std::chrono::steady_clock::now() + timeout
                                                 : std::chrono::steady_clock::time_point::max() };
        if (!m_jobChanged.wait_until(lock, deadline, [&] {
            return terminal(job->info.state) || stop.stop_requested() || m_stopped;
        })) {
            throw std::runtime_error("wait timed out; motion continues");
        }
        if (stop.stop_requested()) {
            throw std::runtime_error("wait interrupted; motion continues");
        }
        if (m_stopped && !terminal(job->info.state)) {
            throw std::runtime_error("motion client stopped; operation outcome unknown");
        }
        return job->info;
    }
    void Client::completeRenewalLocked(std::chrono::steady_clock::time_point now)
    {
        if (m_renewal && m_renewal->ready()) {
            // ready() makes get() nonblocking. Decode on the client worker under
            // its mutex, not in an ICC callback; a failed reply never proves stop.
            const auto response{ m_renewal->get() };
            m_renewalReady = {};
            m_renewal.reset();
            m_leaseStatus.transport_error.reset();
            if (!response) {
                m_leaseStatus.transport_error = response.error();
                m_leaseStatus.error = tr::Error::Unknown;
            }
            else {
                const auto ack{ tr::decode_reply(*response) }; // Includes CRC and wire-version validation.
                if (!ack) {
                    m_leaseStatus.error = ack.error();
                }
                else if (!ack->acknowledgement) {
                    m_leaseStatus.error = tr::Error::Protocol;
                }
                else if (ack->boot != m_renewalBoot || ack->boot != m_boot) {
                    m_leaseStatus.error = tr::Error::Stale;
                }
                else {
                    m_leaseStatus.error = ack->error;
                }
            }
            if (m_leaseStatus.error == tr::Error::None) {
                ++m_leaseStatus.acknowledgements;
                m_leaseStatus.last_acknowledgement = now;
            }
            else {
                ++m_leaseStatus.failures;
                if (now >= m_nextLeaseWarning) {
                    pnm::log::warn("Motion lease acknowledgement failed: {}; IO stop unconfirmed",
                                   tr::name(m_leaseStatus.error));
                    m_nextLeaseWarning = now + 1s;
                }
            }
        }
    }
    void Client::renewJobs()
    {
        std::scoped_lock lock{ m_mutex };
        const auto now{ std::chrono::steady_clock::now() };
        completeRenewalLocked(now);
        if (now < m_nextRenew) {
            return;
        }
        m_nextRenew = now + std::chrono::milliseconds(::motion::operating::OWNER_RENEW_MS);
        if (m_renewal || !m_connected || !m_boot) {
            return;
        }
        tr::Request request{ .operation = tr::Operation::Renew, .boot = m_boot };
        unsigned n{};
        for (const auto& job : m_jobs) {
            if (!terminal(job->info.state) && (job->handle || job->reservation)) {
                request.renew_jobs[n++] = job->info.id;
            }
        }
        if (!n) {
            return;
        }
        m_renewalBoot = request.boot;
        m_renewal.emplace(m_control.request(tr::encode(request), RENEW_TIMEOUT));
        m_renewalReady = pnm::msg::watch(*m_renewal, [n = m_notification] { n->signal(); });
    }
    Client::LeaseStatus Client::leaseStatus() const
    {
        std::scoped_lock lock{ m_mutex };
        return m_leaseStatus;
    }
}
