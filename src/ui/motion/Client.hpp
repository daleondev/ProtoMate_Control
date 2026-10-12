#pragma once

#include "common/motion/Motor.hpp"
#include "runtime/synchronization/Notification.hpp"
#include <pneumo/messaging.hpp>
#include <mutex>
#include <stop_token>

namespace ui::motion
{
    // Application-owned job handles survive the CLI command that submitted them.
    // run() dispatches callbacks on the UI application worker, outside ICC.
    class Client final
    {
      public:
        enum class JobState : std::uint8_t { Submitting, Accepted, Completed, Stopped, TimedOut, Rejected, Faulted, Unknown };
        struct JobInfo
        {
            std::uint64_t id{};
            ::motion::MotorId motor{};
            ::motion::MotorMotion motion{};
            JobState state{ JobState::Submitting };
            ::motion::MotorProgress progress;
        };

        explicit Client(pnm::msg::Bus& bus);
        // Called by the application's ICC connection callback. Losing the link
        // invalidates telemetry immediately; it does not confirm a motor stop.
        void setConnected(bool connected);
        // Latest received state; throws for missing, stale or disconnected data.
        ::motion::MotorSnapshot state() const;
        ::motion::MotorResponse request(const ::motion::MotorRequest& request, std::stop_token stop = {});
        std::uint64_t submit(::motion::MotorGoal goal, std::stop_token stop = {});
        std::vector<JobInfo> jobs() const;
        void run(std::stop_token stop);

      private:
        struct Job
        {
            JobInfo info;
            std::optional<pnm::msg::PendingGoal<::motion::MotorProgress, ::motion::MotorResult>> handle;
            pnm::msg::ReadinessRegistration readiness;
        };
        pnm::msg::Service<::motion::MotorRequest, ::motion::MotorResponse> m_commands, m_urgent;
        pnm::msg::Action<::motion::MotorGoal, ::motion::MotorProgress, ::motion::MotorResult> m_motions;
        pnm::msg::Subscription<::motion::MotorSnapshot> m_stateSubscription;
        std::shared_ptr<runtime::Notification> m_notification{ std::make_shared<runtime::Notification>() };
        mutable std::mutex m_mutex;
        std::vector<std::shared_ptr<Job>> m_jobs;
        std::optional<::motion::MotorSnapshot> m_state;
        std::chrono::steady_clock::time_point m_receivedAt;
        std::optional<std::uint64_t> m_commandGeneration;
        std::uint64_t m_nextId{ 1 };
        bool m_stopped{}, m_connected{};
    };
}
