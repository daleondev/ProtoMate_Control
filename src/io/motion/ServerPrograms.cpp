#include "Server.hpp"
#include <algorithm>
#include <pneumo/logging.hpp>

namespace io::motion
{
    namespace tr = ::motion::trajectory;
    void Server::deferProgram(const tr::Packet& packet, ProgramReply reply)
    {
        const auto request = tr::decodeRequest(packet);
        if (!request) {
            reply.respond(tr::encode(tr::Reply{ .error = request.error() }));
            return;
        }
        if (m_stop.stop_requested()) {
            reply.fail(pnm::msg::ServiceError::Cancelled);
            return;
        }
        if (request->operation == tr::Operation::Inspect || request->operation == tr::Operation::Acquire) {
            auto result = m_controller->control(*request);
            result.generation = m_commandGeneration;
            reply.respond(tr::encode(result));
            return;
        }
        if (request->operation != tr::Operation::Prepare) {
            reply.respond(tr::encode(tr::Reply{ .error = tr::Error::Protocol }));
            return;
        }
        finishProgramCommand();
        std::scoped_lock lock{ m_commandMutex };
        if (m_command || m_programCommand) {
            reply.fail(pnm::msg::ServiceError::Busy);
            return;
        }
        m_programCommand = std::make_shared<ProgramCommand>(*request, std::move(reply));
        pnm::log::debug("Queued preparation {}", request->job);
        m_programCommand->cancellation =
          pnm::msg::watch(m_programCommand->reply, [weak = std::weak_ptr{ m_programCommand }] {
            if (const auto pending = weak.lock(); pending && !pending->reply.pending()) {
                pending->stop.request_stop();
            }
        });
        m_commandNotification.signal();
    }
    void Server::finishProgramCommand()
    {
        std::shared_ptr<ProgramCommand> command;
        {
            std::scoped_lock lock{ m_commandMutex };
            if (!m_programCommand || !m_programCommand->done.load()) {
                return;
            }
            command = std::exchange(m_programCommand, {});
        }
        if (!command->reply.respond(command->result)) {
            pnm::log::warn("Preparation {} reply no longer pending", command->request.job);
            auto release = command->request;
            release.operation = tr::Operation::Release;
            m_controller->control(release);
        }
        else {
            pnm::log::debug("Preparation {} replied", command->request.job);
        }
    }
    void Server::admitProgram(const tr::Run& run, ProgramExecution execution)
    {
        const auto release = [&] {
            tr::Request request{ .operation = tr::Operation::Release,
                                 .boot = run.boot,
                                 .session = run.session };
            request.job = run.job;
            m_controller->control(request);
        };
        auto* slot = std::ranges::find_if(m_programJobs, [](const auto& job) { return !job; });
        if (slot == m_programJobs.end()) {
            execution.fail(pnm::msg::ActionError::Busy);
            release();
            return;
        }
        if (m_stop.stop_requested() || execution.cancelRequested()) {
            execution.cancelled(
              { .job = run.job, .state = ::motion::JobState::Stopped, .error = tr::Error::Cancelled });
            release();
            return;
        }
        auto finished = std::make_shared<ProgramFinished>();
        auto cancellation = pnm::msg::watch(execution, [n = m_notification] { n->signal(); });
        const auto error = m_controller->startProgram(run, [finished, n = m_notification](tr::Update result) {
            finished->result = result;
            finished->ready.store(true);
            n->signal();
        });
        if (error != tr::Error::None) {
            execution.reject({ .job = run.job, .state = ::motion::JobState::Rejected, .error = error });
            release();
            return;
        }
        if (!execution.accept()) {
            tr::Request stop{ .operation = tr::Operation::Stop, .boot = run.boot, .session = run.session };
            stop.job = run.job;
            m_controller->control(stop);
        }
        slot->emplace(ProgramJob{ .run = run,
                                  .execution = std::move(execution),
                                  .finished = finished,
                                  .cancellation = std::move(cancellation),
                                  .stopping = false });
    }
    void Server::dispatchPrograms()
    {
        for (auto& job : m_programJobs) {
            if (!job) {
                continue;
            }
            if (job->finished->ready.load()) {
                const auto result = job->finished->result;
                if (result.state == ::motion::JobState::Completed) {
                    job->execution.succeed(result);
                }
                else if (result.state == ::motion::JobState::Stopped) {
                    job->execution.cancelled(result);
                }
                else {
                    job->execution.abort(result);
                }
                m_controller->releaseJob(job->run.job);
                job.reset();
            }
            else if (!job->stopping && job->execution.cancelRequested()) {
                tr::Request stop{ .operation = tr::Operation::Stop,
                                  .boot = job->run.boot,
                                  .session = job->run.session };
                stop.job = job->run.job;
                m_controller->control(stop);
                job->stopping = true;
            }
        }
    }
}
