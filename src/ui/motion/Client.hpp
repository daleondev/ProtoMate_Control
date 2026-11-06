#pragma once

#include "common/motion/Motor.hpp"
#include "common/motion/Trajectory.hpp"
#include "runtime/synchronization/Notification.hpp"
#include <condition_variable>
#include <mutex>
#include <pneumo/messaging.hpp>
#include <stop_token>

namespace ui::motion
{
    // Application-owned job handles survive the CLI command that submitted them.
    // run() dispatches callbacks on the UI application worker, outside ICC.
    class Client final
    {
      public:
        enum class JobState : std::uint8_t
        {
            Submitting,
            Planning,
            Preparing,
            Accepted,
            Running,
            Stopping,
            Completed,
            Stopped,
            TimedOut,
            Rejected,
            Faulted,
            Unknown
        };
        enum class RequestSpace : std::uint8_t
        {
            None,
            Shaft,
            Rotary,
            Linear,
            Joints,
            Point,
            Home,
            Jog
        };
        struct Requested
        {
            std::array<double, 3> values{};
            RequestSpace space{};
            bool relative{};
        };
        struct JobInfo
        {
            std::uint64_t id{};
            ::motion::MotorId motor{};
            ::motion::MotorMotion motion{};
            JobState state{ JobState::Submitting };
            ::motion::MotorProgress progress;
            ::motion::Family family{ ::motion::Family::Motor };
            std::uint8_t axes{};
            ::motion::trajectory::Error error{};
            std::uint64_t busy_job{};
            ::motion::trajectory::Counts endpoint{};
            double planned_seconds{};
            Requested requested{};
        };
        struct LeaseStatus
        {
            std::uint64_t acknowledgements{}, failures{};
            ::motion::trajectory::Error error{};
            std::optional<pnm::msg::ServiceError> transport_error;
            std::optional<std::chrono::steady_clock::time_point> last_acknowledgement;
        };

        explicit Client(pnm::msg::Bus& bus);
        ~Client();
        void finishPlanning(std::uint64_t id);
        void setPlannedDuration(std::uint64_t id, double seconds);
        void describe(std::uint64_t id, Requested request);
        // Called by the application's ICC connection callback. Losing the link
        // invalidates telemetry immediately; it does not confirm a motor stop.
        void setConnected(bool connected);
        // Latest received state; throws for missing, stale or disconnected data.
        ::motion::MotorSnapshot state() const;
        ::motion::MotorResponse request(const ::motion::MotorRequest& request, std::stop_token stop = {});
        std::uint64_t submit(::motion::MotorGoal goal, std::stop_token stop = {});
        std::vector<JobInfo> jobs() const;
        unsigned copyJobs(unsigned offset, std::span<JobInfo> output) const;
        using Completion =
          std::function<void(std::expected<::motion::MotorResponse, pnm::msg::ServiceError>)>;
        // Bounded normal requests; completion runs on run(), never the caller or ICC.
        bool requestAsync(const ::motion::MotorRequest&, Completion completion);
        void stopAsync(std::uint8_t axes = 7, bool abort = false);
        void cancelAsync(std::uint64_t id);
        void superviseJog(std::uint64_t id);
        void renewJogInput(std::uint64_t id);
        void releaseJogInput();
        void notify() { m_notification->signal(); }
        void setObserver(std::function<void()> observer); // install before run(), cleared after join
        std::chrono::steady_clock::time_point receivedAt() const;
        LeaseStatus leaseStatus() const;
        std::uint64_t create(::motion::Family family, std::uint8_t axes);
        std::stop_token cancellation(std::uint64_t id) const;
        void reject(std::uint64_t id, ::motion::trajectory::Error error, std::uint64_t busy_job = 0);
        ::motion::trajectory::Reply inspect(std::stop_token stop = {});
        ::motion::trajectory::Reply acquire(std::uint64_t id,
                                            std::uint64_t configuration,
                                            std::stop_token stop = {},
                                            std::uint8_t axes = 0);
        void execute(std::uint64_t id,
                     const ::motion::trajectory::Reply& reservation,
                     std::uint64_t configuration,
                     const ::motion::trajectory::FiniteProgram& program,
                     std::stop_token stop = {});
        void cancel(std::uint64_t id);
        void cancelFamily(::motion::Family family);
        void stop(std::uint8_t axes = 7, bool abort = false);
        JobInfo wait(std::uint64_t id, std::chrono::milliseconds timeout, std::stop_token stop = {});
        void run(std::stop_token stop);

      private:
        struct Job
        {
            JobInfo info;
            std::optional<pnm::msg::PendingGoal<::motion::MotorProgress, ::motion::MotorResult>> handle;
            pnm::msg::ReadinessRegistration readiness;
            std::optional<pnm::msg::PendingGoal<::motion::trajectory::Update, ::motion::trajectory::Update>>
              program;
            std::stop_source stop;
            std::uint64_t boot{}, reservation{};
            bool preparing{};
        };
        struct ControlCall
        {
            ::motion::MotorRequest request;
            Completion completion;
            bool cancelled{};
            std::optional<pnm::msg::PendingCall<::motion::MotorResponse>> handle;
            pnm::msg::ReadinessRegistration readiness;
        };
        std::array<std::optional<ControlCall>, 4> m_calls;
        std::optional<ControlCall> m_disable;
        std::optional<pnm::msg::PendingCall<::motion::trajectory::Packet>> m_stopCall;
        pnm::msg::ReadinessRegistration m_stopReady;
        std::uint8_t m_stopAxes{}, m_abortAxes{};
        std::uint64_t m_inputJob{};
        std::chrono::steady_clock::time_point m_inputDeadline{};
        std::function<void()> m_observer;
        void serviceControls();
        struct History
        {
            std::array<JobInfo, 32> jobs{};
            unsigned count{};
        };
        static_assert(std::is_trivially_copyable_v<History>);
        struct ReleaseHistory
        {
            void operator()(History*) const noexcept;
        };
        static std::unique_ptr<History, ReleaseHistory> reserveHistory();
        std::unique_ptr<History, ReleaseHistory> m_history;
        void retireJobsLocked();
        std::shared_ptr<Job> findLocked(std::uint64_t id) const;
        std::shared_ptr<Job> createLocked(::motion::Family family, std::uint8_t axes);
        ::motion::trajectory::Reply exchange(const ::motion::trajectory::Request&,
                                             bool urgent,
                                             std::stop_token stop = {});
        void renewJobs();
        void releaseReservation(const std::shared_ptr<Job>& job);
        static bool terminal(JobState state) { return state >= JobState::Completed; }
        pnm::msg::Service<::motion::MotorRequest, ::motion::MotorResponse> m_commands, m_urgent;
        pnm::msg::Action<::motion::MotorGoal, ::motion::MotorProgress, ::motion::MotorResult> m_motions;
        pnm::msg::Service<::motion::trajectory::Packet, ::motion::trajectory::Packet> m_trajectory, m_control;
        pnm::msg::
          Action<::motion::trajectory::Run, ::motion::trajectory::Update, ::motion::trajectory::Update>
            m_programs;
        pnm::msg::Subscription<::motion::MotorSnapshot> m_stateSubscription;
        std::shared_ptr<runtime::Notification> m_notification{ std::make_shared<runtime::Notification>() };
        mutable std::mutex m_mutex;
        std::condition_variable m_jobChanged;
        std::vector<std::shared_ptr<Job>> m_jobs;
        std::optional<::motion::MotorSnapshot> m_state;
        std::chrono::steady_clock::time_point m_receivedAt;
        std::optional<std::uint64_t> m_commandGeneration;
        std::uint64_t m_nextId{ 1 };
        bool m_stopped{}, m_connected{};
        std::optional<pnm::msg::PendingCall<::motion::trajectory::Packet>> m_renewal;
        pnm::msg::ReadinessRegistration m_renewalReady;
        std::chrono::steady_clock::time_point m_nextRenew{};
        std::chrono::steady_clock::time_point m_nextLeaseWarning{};
        LeaseStatus m_leaseStatus;
        std::uint64_t m_renewalBoot{};
        std::uint64_t m_boot{};
    };
}
