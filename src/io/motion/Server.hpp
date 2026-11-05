#pragma once

#include "Controller.hpp"
#include "common/motion/Motor.hpp"
#include <pneumo/messaging.hpp>

namespace io::motion
{
    // Driven by the IO application worker, never by the ICC context or an ISR.
    // Stop and join run() before destroying this server or its controller.
    class Server final
    {
      public:
        Server(pnm::msg::Bus& bus, Controller& controller);
        void run(std::stop_token stop);

      private:
        using Execution = pnm::msg::ActionExecution<::motion::MotorProgress, ::motion::MotorResult>;
        struct Finished
        {
            std::atomic_bool ready{};
        };
        struct Job
        {
            ::motion::MotorId motor;
            Execution execution;
            std::shared_ptr<Finished> finished;
            std::future<StepperMotor::Result> result;
            pnm::msg::ReadinessRegistration cancellation;
        };
        using Reply = pnm::msg::Reply<::motion::MotorResponse>;
        struct Command
        {
            Command(const ::motion::MotorRequest& value, Reply response)
              : request{ value }
              , reply{ std::move(response) }
            {
            }
            ::motion::MotorRequest request;
            Reply reply;
            std::stop_source stop;
            pnm::msg::ReadinessRegistration cancellation;
            ::motion::MotorResponse result;
            std::atomic_bool done{};
            bool started{};
        };
        ::motion::MotorResponse request(const ::motion::MotorRequest& request,
                                         bool urgent,
                                         std::stop_token stop);
        void deferCommand(const ::motion::MotorRequest& request, Reply reply);
        void finishCommand();
        void cancelCommand();
        void discardQueuedCommands();
        void commandWorker(std::stop_token stop);
        ::motion::MotorSnapshot snapshot();
        ::motion::MotorProgress progress(::motion::MotorId id) const;
        void admit(const ::motion::MotorGoal& goal, Execution execution);
        void dispatchJobs(bool feedback);

        ::motion::trajectory::Packet trajectory(const ::motion::trajectory::Packet& packet);
        Controller* m_controller;
        std::shared_ptr<runtime::Notification> m_notification{ std::make_shared<runtime::Notification>() };
        std::array<std::optional<Job>, 8> m_jobs;
        std::uint64_t m_commandGeneration{ 1 }, m_sequence{};
        std::stop_token m_stop;
        bool m_discardCommands{}; // Dispatcher-only, while draining pre-stop requests.
        // One bounded ordinary operation; the dispatcher stays available to
        // stop axes, acknowledge cancellations and publish telemetry.
        std::mutex m_commandMutex;
        std::shared_ptr<Command> m_command;
        runtime::Notification m_commandNotification;
        pnm::msg::Topic<::motion::MotorSnapshot> m_state;
        pnm::msg::ServiceServer<::motion::MotorRequest, ::motion::MotorResponse> m_commands, m_urgent;
        pnm::msg::ActionServer<::motion::MotorGoal, ::motion::MotorProgress, ::motion::MotorResult> m_motions;
        pnm::msg::ServiceServer<::motion::trajectory::Packet, ::motion::trajectory::Packet> m_trajectories;
    };
}
