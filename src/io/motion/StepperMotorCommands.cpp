#include "StepperMotor.hpp"
#include "pneumo/logging.hpp"
#include "runtime/thread.hpp"

#include <algorithm>
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

    StepperMotor::Result StepperMotor::performReference(pnm::units::AngularVelocity seek_velocity,
                                                        pnm::units::AngularVelocity latch_velocity,
                                                        pnm::units::Time timeout,
                                                        const std::stop_token& stop)
    {
        const auto motor_id{ static_cast<unsigned>(m_id) + 1U };
        const auto now{ std::chrono::steady_clock::now() };
        if (!m_driver->status().ready || !timingFor(seek_velocity) || !timingFor(latch_velocity) ||
            latch_velocity >= seek_velocity || !timeout.isFinite() || timeout <= 0_s ||
            timeout >= (std::chrono::steady_clock::time_point::max() - now) / 2) {
            pnm::log::debug(logSink(pnm::log::Level::Debug),
                            "Motor {} reference rejected: invalid seek/latch speeds or overall timeout",
                            motor_id);
            return Result::Rejected;
        }

        if (referenceSwitchActive()) {
            pnm::log::warn(logSink(pnm::log::Level::Warn),
                           "Motor {} reference rejected: clear the active limit switch before homing",
                           motor_id);
            return Result::Rejected;
        }
        const bool previous_limit_fault = m_events->limit_fault.exchange(false);
        m_events->referencing.store(true);
        m_events->referenced.store(false);
        struct FinishReferencing
        {
            MotionEvents& events;
            const hal::IDigitalInput& input;
            bool restore_fault;
            bool complete{};
            explicit FinishReferencing(MotionEvents& value,
                                       const hal::IDigitalInput& reference,
                                       bool previous_fault)
              : events{ value }
              , input{ reference }
              , restore_fault{ previous_fault }
            {
            }
            FinishReferencing(const FinishReferencing&) = delete;
            FinishReferencing& operator=(const FinishReferencing&) = delete;
            FinishReferencing(FinishReferencing&&) = delete;
            FinishReferencing& operator=(FinishReferencing&&) = delete;
            ~FinishReferencing()
            {
                if (!complete) {
                    events.referenced.store(false);
                    if (restore_fault)
                        events.limit_fault.store(true);
                }
                events.referencing.store(false);
                // Cancellation/timeout can leave the switch held without a new
                // GPIO edge. Once homing ends, that is an ordinary hard limit.
                if (input.read() == hal::gpio::Level::High) {
                    events.referenced.store(false);
                    events.limit_fault.store(true);
                    if (events.shutdown)
                        events.shutdown();
                    events.notification.signal();
                    if (auto* group = events.group_notification.load())
                        group->signal();
                }
            }
        } finish{ *m_events, *m_referenceSwitchInput, previous_limit_fault };
        // Catch activation racing the transition into homing; do not claim a
        // switch that was active before the seek as a new reference.
        if (referenceSwitchActive() || m_events->limit_fault.load())
            return Result::Rejected;

        const auto deadline{ now + timeout.toChrono<std::chrono::steady_clock::duration>() };
        const std::stop_callback cancellation{ stop, [this] { m_events->notification.signal(); } };
        const auto seek{ [&](Direction direction, pnm::units::AngularVelocity speed, hal::gpio::Level level) {
            if (interrupted(stop)) {
                return Result::Stopped;
            }
            const auto remaining{ deadline - std::chrono::steady_clock::now() };
            if (remaining <= std::chrono::steady_clock::duration::zero()) {
                return Result::TimedOut;
            }
            return performMotion(direction, speed, pnm::units::Time{ remaining }, stop, std::nullopt, level);
        } };

        const auto seek_direction = m_referenceDirection;
        const auto release_direction =
          seek_direction == Direction::Forward ? Direction::Backward : Direction::Forward;
        using enum hal::gpio::Level;
        pnm::log::debug(logSink(pnm::log::Level::Debug),
                        "Motor {} reference: seeking first contact {} at {} rpm",
                        motor_id,
                        seek_direction == Direction::Forward ? "Forward" : "Backward",
                        seek_velocity.get<pnm::units::AngularVelocityUnits::rpm>());
        const auto first = seek(seek_direction, seek_velocity, High);
        if (first != Result::Completed)
            return first;

        pnm::log::debug(logSink(pnm::log::Level::Debug),
                        "Motor {} reference: confirming first contact (10 ms stable HIGH)",
                        motor_id);
        auto result{ waitReferenceLevel(High, deadline, stop) };
        if (result != Result::Completed) {
            return result;
        }

        pnm::log::debug(logSink(pnm::log::Level::Debug),
                        "Motor {} reference: backing off {} at {} rpm until release",
                        motor_id,
                        release_direction == Direction::Forward ? "Forward" : "Backward",
                        seek_velocity.get<pnm::units::AngularVelocityUnits::rpm>());
        result = seek(release_direction, seek_velocity, Low);
        if (result != Result::Completed) {
            return result;
        }

        pnm::log::debug(logSink(pnm::log::Level::Debug),
                        "Motor {} reference: confirming release (10 ms stable LOW)",
                        motor_id);
        result = waitReferenceLevel(Low, deadline, stop);
        if (result != Result::Completed) {
            return result;
        }

        pnm::log::debug(logSink(pnm::log::Level::Debug),
                        "Motor {} reference: seeking second contact {} at {} rpm",
                        motor_id,
                        seek_direction == Direction::Forward ? "Forward" : "Backward",
                        latch_velocity.get<pnm::units::AngularVelocityUnits::rpm>());
        result = seek(seek_direction, latch_velocity, High);
        if (result != Result::Completed) {
            return result;
        }

        pnm::log::debug(logSink(pnm::log::Level::Debug),
                        "Motor {} reference: confirming second contact (10 ms stable HIGH)",
                        motor_id);
        result = waitReferenceLevel(High, deadline, stop);
        if (result != Result::Completed) {
            return result;
        }

        pnm::log::debug(logSink(pnm::log::Level::Debug),
                        "Motor {} reference: applying switch position {} deg",
                        motor_id,
                        m_referenceSwitchPosition.get<pnm::units::AngleUnits::deg>());
        result = applyReferencePosition(stop);
        if (result != Result::Completed)
            return result;
        pnm::log::debug(logSink(pnm::log::Level::Debug),
                        "Motor {} reference: releasing switch after latch; retaining latched coordinate",
                        motor_id);
        result = seek(release_direction, latch_velocity, Low);
        if (result != Result::Completed)
            return result;
        result = waitReferenceLevel(Low, deadline, stop);
        if (result != Result::Completed || interrupted(stop))
            return result == Result::Completed ? Result::Stopped : result;
        m_events->referencing.store(false);
        if (referenceSwitchActive() || m_events->limit_fault.load())
            return Result::Faulted;
        finish.complete = true;
        return Result::Completed;
    }

    StepperMotor::Result StepperMotor::waitReferenceLevel(hal::gpio::Level level,
                                                          std::chrono::steady_clock::time_point deadline,
                                                          const std::stop_token& stop)
    {
        constexpr auto settle_time{ 10ms };
        std::optional<std::chrono::steady_clock::time_point> stable_since{};
        auto changes{ m_events->reference_changes.load() };
        while (true) {
            if (!m_driver->status().ready) {
                return Result::Faulted;
            }
            if (interrupted(stop)) {
                return Result::Stopped;
            }

            const auto now{ std::chrono::steady_clock::now() };
            if (now >= deadline) {
                return Result::TimedOut;
            }

            const auto current_changes{ m_events->reference_changes.load() };
            const auto current_level{ m_referenceSwitchInput->read() };
            if (current_changes != changes || current_level != level) {
                stable_since.reset();
            }

            if (!stable_since && current_level == level) {
                stable_since = now;
            }

            changes = current_changes;
            if (stable_since && now >= *stable_since + settle_time &&
                m_events->reference_changes.load() == current_changes) {
                return Result::Completed;
            }

            static_cast<void>(m_events->notification.waitUntil(
              stable_since ? std::min(deadline, *stable_since + settle_time) : deadline));
        }
    }

}
