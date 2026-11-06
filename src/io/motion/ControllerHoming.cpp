#include "Controller.hpp"
#include <exception>
#include <pneumo/logging.hpp>
namespace io::motion
{
    void Controller::serviceHomeLocked(Group& g)
    {
        namespace tr = ::motion::trajectory;
        const auto* owner = m_ownership.find(g.job);
        const auto now = Ownership::Clock::now();
        const bool lease_expired = owner && now >= owner->deadline;
        const bool timed_out = now >= g.timeout;
        if (!owner || owner->state == ::motion::JobState::Stopping || lease_expired || timed_out ||
            m_faulted.load()) {
            if (g.error == tr::Error::None) {
                if (m_faulted.load())
                    g.error = tr::Error::Fault;
                else if (lease_expired)
                    g.error = tr::Error::LeaseExpired;
                else if (timed_out)
                    g.error = tr::Error::HomingTimeout;
                else
                    g.error = tr::Error::Cancelled;
            }
            for (unsigned i = 0; i < 3; ++i) {
                if (g.mask & (1U << i))
                    m_motors[i]->requestStop();
            }
        }
        if (g.home_result.valid()) {
            if (!g.home_ready.load()) {
                return;
            }
            const auto motor = g.home_order[g.home_stage];
            auto result = StepperMotor::Result::Faulted;
            try {
                result = g.home_result.get();
            } catch (...) {
            }
            m_motors[motor]->stopAndWait();
            if (result != StepperMotor::Result::Completed && g.error == tr::Error::None) {
                g.error = result == StepperMotor::Result::Faulted    ? tr::Error::Fault
                          : result == StepperMotor::Result::TimedOut ? tr::Error::HomingTimeout
                                                                     : tr::Error::Cancelled;
            }
            ++g.home_stage;
        }
        if (g.error != tr::Error::None) {
            g.home_finished = true;
            return;
        }
        while (g.home_stage < 3 && !(g.mask & (1U << g.home_order[g.home_stage]))) {
            ++g.home_stage;
        }
        if (g.home_stage == 3) {
            g.home_finished = true;
            return;
        }
        auto& motor = *m_motors[g.home_order[g.home_stage]];
        const auto& parameters = g.home[g.home_order[g.home_stage]];
        g.home_ready = false;
        try {
            g.home_result = motor.reference(
              parameters.seek_rpm * 1_rpm, parameters.latch_rpm * 1_rpm, parameters.timeout * 1_s);
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
                                unsigned(g.home_order[g.home_stage]) + 1U,
                                error.what());
            } catch (...) {
            } // A low-memory failure must not depend on logging.
        } catch (...) {
            g.error = tr::Error::Fault;
            g.home_finished = true;
            try {
                pnm::log::error("Homing job {} could not start motor {}: unknown exception",
                                g.job,
                                unsigned(g.home_order[g.home_stage]) + 1U);
            } catch (...) {
            }
        }
    }
}
