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
        struct Finished { std::atomic_bool ready{}; };
        struct Job
        {
            ::motion::MotorId motor;
            Execution execution;
            std::shared_ptr<Finished> finished;
            std::future<StepperMotor::Result> result;
            pnm::msg::ReadinessRegistration cancellation;
        };
        ::motion::MotorResponse request(const ::motion::MotorRequest& request, bool urgent);
        ::motion::MotorResponse snapshot() const;
        ::motion::MotorProgress progress(::motion::MotorId id) const;
        void admit(const ::motion::MotorGoal& goal, Execution execution);
        void dispatchJobs(bool feedback);

        Controller* m_controller;
        std::shared_ptr<runtime::Notification> m_notification{ std::make_shared<runtime::Notification>() };
        std::array<std::optional<Job>, 8> m_jobs;
        std::uint64_t m_generation{ 1 };
        std::stop_token m_stop;
        pnm::msg::ServiceServer<::motion::MotorRequest, ::motion::MotorResponse> m_commands, m_urgent;
        pnm::msg::ActionServer<::motion::MotorGoal, ::motion::MotorProgress, ::motion::MotorResult> m_motions;
    };
}
