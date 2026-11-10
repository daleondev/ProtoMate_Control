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
        void run(const std::stop_token& stop);
        void peerLost(bool stopping) noexcept
        {
            if (stopping) {
                m_peerStopping = true;
            }
            m_peerLost = true;
            m_notification->signal();
        }

      private:
        using Execution = pnm::msg::ActionExecution<::motion::MotorProgress, ::motion::MotorResult>;
        struct Finished
        {
            std::atomic_bool ready{};
        };
        struct Job
        {
            ::motion::MotorId motor;
            std::uint64_t id{};
            Execution execution;
            std::shared_ptr<Finished> finished;
            std::future<StepperMotor::Result> result;
            pnm::msg::ReadinessRegistration cancellation;
        };
        using ProgramReply = pnm::msg::Reply<::motion::trajectory::Packet>;
        using ProgramExecution =
          pnm::msg::ActionExecution<::motion::trajectory::Update, ::motion::trajectory::Update>;
        struct ProgramCommand
        {
            ::motion::trajectory::Request request;
            ProgramReply reply;
            std::stop_source stop;
            pnm::msg::ReadinessRegistration cancellation;
            ::motion::trajectory::Packet result;
            std::atomic_bool done{};
            bool started{};
            ProgramCommand(::motion::trajectory::Request r, ProgramReply response)
              : request{ r }
              , reply{ std::move(response) }
            {
            }
        };
        struct ProgramFinished
        {
            ::motion::trajectory::Update result{};
            std::atomic_bool ready{};
        };
        struct ProgramJob
        {
            ::motion::trajectory::Run run;
            ProgramExecution execution;
            std::shared_ptr<ProgramFinished> finished;
            pnm::msg::ReadinessRegistration cancellation;
            bool stopping{};
        };
        void deferProgram(const ::motion::trajectory::Packet& packet, ProgramReply reply);
        void admitProgram(const ::motion::trajectory::Run& run, ProgramExecution execution);
        void dispatchPrograms();
        void finishProgramCommand();
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
        ::motion::MotorResponse request(const ::motion::MotorRequest& value,
                                        bool urgent,
                                        const std::stop_token& stop);
        void deferCommand(const ::motion::MotorRequest& value, Reply reply);
        void finishCommand();
        void cancelCommand();
        void discardQueuedCommands();
        void prepareCommand(ProgramCommand& program);
        void commandWorker(const std::stop_token& stop);
        ::motion::MotorSnapshot snapshot();
        ::motion::MotorProgress progress(::motion::MotorId motor) const;
        void admit(const ::motion::MotorGoal& goal, Execution execution);
        void dispatchJobs(bool feedback);

        ::motion::trajectory::Packet trajectory(const ::motion::trajectory::Packet& packet);
        Controller* m_controller;
        std::shared_ptr<runtime::Notification> m_notification{ std::make_shared<runtime::Notification>() };
        std::array<std::optional<Job>, ::motion::MOTOR_JOB_CAPACITY> m_jobs;
        std::array<std::optional<ProgramJob>, 3> m_programJobs;
        std::uint64_t m_commandGeneration{ 1 }, m_sequence{};
        std::stop_token m_stop;
        std::atomic_bool m_peerLost{}, m_peerStopping{};
        bool m_discardCommands{}; // Dispatcher-only, while draining pre-stop requests.
        // One bounded ordinary operation; the dispatcher stays available to
        // stop axes, acknowledge cancellations and publish telemetry.
        std::mutex m_commandMutex;
        std::shared_ptr<Command> m_command;
        std::shared_ptr<ProgramCommand> m_programCommand;
        runtime::Notification m_commandNotification;
        pnm::msg::Topic<::motion::MotorSnapshot> m_state;
        pnm::msg::ServiceServer<::motion::MotorRequest, ::motion::MotorResponse> m_commands, m_urgent;
        pnm::msg::ActionServer<::motion::MotorGoal, ::motion::MotorProgress, ::motion::MotorResult> m_motions;
        pnm::msg::ServiceServer<::motion::trajectory::Packet, ::motion::trajectory::Packet> m_trajectories;
        pnm::msg::ServiceServer<::motion::trajectory::Packet, ::motion::trajectory::Packet>
          m_trajectoryControl;
        pnm::msg::
          ActionServer<::motion::trajectory::Run, ::motion::trajectory::Update, ::motion::trajectory::Update>
            m_programs;
    };
}
