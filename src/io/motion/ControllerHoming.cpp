#include "Controller.hpp"
#include <exception>
#include <pneumo/logging.hpp>
namespace io::motion
{
    namespace
    {
        constexpr unsigned AXIS_COUNT{ 3 }, HOME_AND_NEUTRAL_STAGES{ 2 * AXIS_COUNT };
        ::motion::trajectory::Error home_result_error(StepperMotor::Result result, bool seeking)
        {
            namespace tr = ::motion::trajectory;
            switch (result) {
                case StepperMotor::Result::Faulted:
                    return tr::Error::Fault;
                case StepperMotor::Result::Rejected:
                    return seeking ? tr::Error::ReferenceActive : tr::Error::Configuration;
                case StepperMotor::Result::TimedOut:
                    return tr::Error::HomingTimeout;
                default:
                    return tr::Error::Cancelled;
            }
        }
    }
    void Controller::cancelExpiredHomeLocked(Group& g)
    {
        namespace tr = ::motion::trajectory;
        const auto* owner{ m_ownership.find(g.job) };
        const auto now{ Ownership::Clock::now() };
        const bool lease_expired{ owner && now >= owner->deadline };
        const bool timed_out{ now >= g.timeout };
        if (!owner || owner->state == ::motion::JobState::Stopping || lease_expired || timed_out ||
            m_faulted.load()) {
            if (g.error == tr::Error::None) {
                if (m_faulted.load()) {
                    g.error = tr::Error::Fault;
                }
                else if (lease_expired) {
                    g.error = tr::Error::LeaseExpired;
                }
                else if (timed_out) {
                    g.error = tr::Error::HomingTimeout;
                }
                else {
                    g.error = tr::Error::Cancelled;
                }
            }
            stopMotorsLocked(g.mask, false);
        }
    }
    bool Controller::finishHomeStageLocked(Group& g)
    {
        namespace tr = ::motion::trajectory;
        if (g.home_result.valid()) {
            if (!g.home_ready.load()) {
                return false;
            }
            const auto motor{ g.home_order[g.home_stage % AXIS_COUNT] };
            auto result{ StepperMotor::Result::Faulted };
            try {
                result = g.home_result.get();
            } catch (...) {
                result = StepperMotor::Result::Faulted;
            }
            m_motors[motor]->stopAndWait();
            if (result != StepperMotor::Result::Completed && g.error == tr::Error::None) {
                g.error = home_result_error(result, g.home_stage < AXIS_COUNT);
            }
            ++g.home_stage;
        }
        return true;
    }
    void Controller::startHomeStageLocked(Group& g)
    {
        namespace tr = ::motion::trajectory;
        auto& motor{ *m_motors[g.home_order[g.home_stage % AXIS_COUNT]] };
        const auto& parameters{ g.home[g.home_order[g.home_stage % AXIS_COUNT]] };
        g.home_ready = false;
        try {
            if (g.home_stage < AXIS_COUNT) {
                g.home_result = motor.reference(
                  parameters.seek_rpm * 1_rpm, parameters.latch_rpm * 1_rpm, parameters.timeout * 1_s);
            }
            else {
                const auto index{ g.home_order[g.home_stage % AXIS_COUNT] };
                const auto& limits{ g.home_limits[index] };
                const auto position{ motor.setpointPosition() / motor.m_stepAngle };
                if (!motor.isReferenced() || position < limits.minimum || position > limits.maximum) {
                    g.error = tr::Error::PositionLimit;
                    g.home_finished = true;
                    return;
                }
                g.home_result = motor.moveAbs(motor.m_stepAngle * double(g.home_neutral[index]),
                                              motor.m_stepAngle * limits.velocity / 1_s,
                                              motor.m_stepAngle * limits.acceleration / 1_s / 1_s,
                                              motor.m_stepAngle * limits.deceleration / 1_s / 1_s,
                                              motor.m_stepAngle * limits.jerk / 1_s / 1_s / 1_s,
                                              BufferMode::Aborting,
                                              ::motion::operating::MAX_PROGRAM_SECONDS * 1_s);
            }
            motor.m_lastCompletion.then([this, &g] {
                g.home_ready = true;
                m_executionNotification.signal();
            });
        } catch (const std::exception& error) {
            g.error = tr::Error::Fault;
            g.home_finished = true;
            try {
                pnm::log::error("Homing job {} could not start motor {}: {}",
                                g.job,
                                unsigned(g.home_order[g.home_stage % AXIS_COUNT]) + 1U,
                                error.what());
            } catch (...) { // NOLINT(bugprone-empty-catch): The job is already faulted; logging failure must
                            // not interrupt cleanup.

            } // A low-memory failure must not depend on logging.
        } catch (...) {
            g.error = tr::Error::Fault;
            g.home_finished = true;
            try {
                pnm::log::error("Homing job {} could not start motor {}: unknown exception",
                                g.job,
                                unsigned(g.home_order[g.home_stage % AXIS_COUNT]) + 1U);
            } catch (...) { // NOLINT(bugprone-empty-catch): The job is already faulted; logging failure must
                            // not interrupt cleanup.
            }
        }
    }
    void Controller::serviceHomeLocked(Group& g)
    {
        namespace tr = ::motion::trajectory;
        cancelExpiredHomeLocked(g);
        if (!finishHomeStageLocked(g)) {
            return;
        }
        if (g.error != tr::Error::None) {
            g.home_finished = true;
            return;
        }
        while (g.home_stage < AXIS_COUNT && !(g.mask & (1U << g.home_order[g.home_stage % AXIS_COUNT]))) {
            ++g.home_stage;
        }
        if (g.home_stage == (g.home_to_neutral ? HOME_AND_NEUTRAL_STAGES : AXIS_COUNT)) {
            g.home_finished = true;
            return;
        }
        startHomeStageLocked(g);
    }
}
