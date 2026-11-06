#include "StepperMotor.hpp"
#include "pneumo/logging.hpp"
#include "runtime/thread.hpp"

#include <cstdio>
#include <exception>

namespace io::motion
{
    void StepperMotor::reportOperationFailure(const char* stage, std::error_code error) const noexcept
    {
        try {
            pnm::log::error(logSink(pnm::log::Level::Error),
                            "Motor {} {} failed: {} (errno={})",
                            static_cast<unsigned>(m_id) + 1U,
                            stage,
                            error.message(),
                            error.value());
        } catch (...) {
            std::fputs("Motor operation failed; diagnostic logging unavailable\n", stderr);
        }
    }

    // Cold admission and failure reporting stay outside the instruction-RAM
    // execution paths. Never called from an interrupt.
    void StepperMotor::reportFailure(const char* stage) const noexcept
    {
        const auto failure = std::current_exception();
        try {
            try {
                if (failure) {
                    std::rethrow_exception(failure);
                }
            } catch (const std::exception& error) {
                pnm::log::error(logSink(pnm::log::Level::Error),
                                "Motor {} {} failed: {}",
                                static_cast<unsigned>(m_id) + 1,
                                stage,
                                error.what());
                return;
            }
            pnm::log::error(logSink(pnm::log::Level::Error),
                            "Motor {} {} failed: unknown exception",
                            static_cast<unsigned>(m_id) + 1,
                            stage);
        } catch (...) {
            std::fputs("Motor failure; diagnostic logging unavailable\n", stderr);
        }
    }

    std::future<StepperMotor::Result> StepperMotor::submitMove(pnm::units::Angle position,
                                                               pnm::units::AngularVelocity velocity,
                                                               Defaults dynamics,
                                                               BufferMode mode,
                                                               pnm::units::Time timeout,
                                                               bool relative)
    {
        const auto epoch{ m_abortEpoch.load() };
        const auto rejected{ [this] {
            util::Completion<Result> promise{};
            m_lastCompletion = promise;
            promise.setValue(Result::Rejected);
            return promise.getFuture();
        } };
        const auto now{ std::chrono::steady_clock::now() };
        if (!m_driver->status().ready || !position.isFinite() || !timingFor(velocity) ||
            !timeout.isFinite() || timeout < 0_s ||
            timeout >= (std::chrono::steady_clock::time_point::max() - now) / 2 ||
            !dynamics.acceleration.isFinite() || dynamics.acceleration < 0_rad_s2 ||
            !dynamics.deceleration.isFinite() || dynamics.deceleration < 0_rad_s2 ||
            !dynamics.jerk.isFinite() || dynamics.jerk < 0_rad_s3 || mode < BufferMode::Aborting ||
            mode > BufferMode::BlendingHigh || (!relative && !isReferenced())) {
            return rejected();
        }
        std::scoped_lock worker_lock{ m_workerMutex };
        {
            std::scoped_lock lock{ m_queueMutex };
            if (dynamics.acceleration == 0_rad_s2) {
                dynamics.acceleration = m_motionDefaults.acceleration;
            }
            if (dynamics.deceleration == 0_rad_s2) {
                dynamics.deceleration = m_motionDefaults.deceleration;
            }
            if (dynamics.jerk == 0_rad_s3) {
                dynamics.jerk = m_motionDefaults.jerk;
            }
        }
        auto command{ std::make_shared<Command>(Command{ .position = position,
                                                         .velocity = velocity,
                                                         .dynamics = dynamics,
                                                         .mode = mode,
                                                         .timeout = timeout,
                                                         .relative = relative,
                                                         .completion = {},
                                                         .epoch = epoch }) };
        m_lastCompletion = command->completion;
        auto future{ command->completion.getFuture() };
        if (mode != BufferMode::Aborting) {
            std::scoped_lock lock{ m_queueMutex };
            if (m_busy) {
                if (m_homing || m_pending) {
                    command->completion.setValue(Result::Rejected);
                    return future;
                }
                m_pending = command;
                m_events->notification.signal();
                return future;
            }
        }
        if (m_worker.joinable()) {
            static_cast<void>(m_worker.request_stop());
            m_worker.join();
        }
        {
            std::scoped_lock lock{ m_queueMutex };
            m_busy = true;
            m_homing = false;
        }
        try {
            m_worker = runtime::thread::create_jthread(
              { .name = "motion", .stack_size = WORKER_STACK_BYTES, .stack_storage = m_workerStack.get() },
              [this, command](const std::stop_token& stop) { runCommands(command, stop); });
        } catch (...) {
            reportFailure("raw worker startup");
            command->completion.setException(std::current_exception());
            finishQueue(Result::Faulted);
        }
        return future;
    }

    std::future<StepperMotor::Result> StepperMotor::startMotion(std::function<Result(std::stop_token)> task)
    {
        const auto epoch{ m_abortEpoch.load() };
        std::scoped_lock lock{ m_workerMutex };
        if (m_worker.joinable()) {
            static_cast<void>(m_worker.request_stop());
            m_worker.join();
        }
        util::Completion<Result> completion{};
        m_lastCompletion = completion;
        auto future{ completion.getFuture() };
        {
            std::scoped_lock queue_lock{ m_queueMutex };
            m_busy = m_homing = true;
        }
        try {
            // Joining the predecessor makes the reserved stack available for
            // reuse, including after homing cancellation.
            m_worker = runtime::thread::create_jthread(
              { .name = "homing", .stack_size = WORKER_STACK_BYTES, .stack_storage = m_workerStack.get() },
              [this, task = std::move(task), completion, epoch](const std::stop_token& stop) mutable {
                m_runEpoch = epoch;
                Result result{ Result::Faulted };
                try {
                    result = task(stop);
                } catch (...) {
                    reportFailure("homing execution");
                    result = Result::Faulted;
                }
                finishQueue(Result::Stopped);
                completion.setValue(result);
                // Publish completion before waking the coordinator, including when
                // homing ended without another STEP or GPIO callback.
                if (auto* notification{ m_events->group_notification.load() }) {
                    notification->signal();
                }
            });
        } catch (...) {
            reportFailure("homing worker startup");
            finishQueue(Result::Faulted);
            throw;
        }
        return future;
    }

}
