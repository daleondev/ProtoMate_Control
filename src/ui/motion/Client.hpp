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
        Client(const Client&) = delete;
        auto operator=(const Client&) -> Client& = delete;
        Client(Client&&) = delete;
        auto operator=(Client&&) -> Client& = delete;

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
            std::uint64_t id{}, source{}; // zero: local GUI/CLI; otherwise a remote control generation
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
        ::motion::MotorResponse request(const ::motion::MotorRequest& value,
                                        const std::stop_token& stop = {});
        std::uint64_t submit(::motion::MotorGoal goal, const std::stop_token& stop = {}, std::uint64_t source = 0);
        std::vector<JobInfo> jobs() const;
        unsigned copyJobs(unsigned offset, std::span<JobInfo> output) const;
        using Completion =
          std::function<void(std::expected<::motion::MotorResponse, pnm::msg::ServiceError>)>;
        // Bounded normal requests; completion runs on run(), never the caller or ICC.
        bool requestAsync(const ::motion::MotorRequest& /*value*/, Completion completion, std::uint64_t source = 0);
        void stopAsync(std::uint8_t axes = ALL_AXES, bool abort = false);
        void cancelAsync(std::uint64_t id);
        void superviseJog(std::uint64_t id);
        void renewJogInput(std::uint64_t id);
        void releaseJogInput();
        void remotePermission(bool enabled); // local frontends only
        bool remotePermission() const;
        std::uint64_t openRemote();
        bool renewRemote(std::uint64_t source, std::span<const std::uint64_t> heldJobs);
        void closeRemote(std::uint64_t source);
        bool remoteAuthorized(std::uint64_t source) const;
        void notify() { m_notification->signal(); }
        void setObserver(std::function<void()> observer); // install before run(), cleared after join
        std::chrono::steady_clock::time_point receivedAt() const;
        LeaseStatus leaseStatus() const;
        std::uint64_t create(::motion::Family family, std::uint8_t axes, std::uint64_t source = 0, bool held = false);
        std::stop_token cancellation(std::uint64_t id) const;
        void reject(std::uint64_t id, ::motion::trajectory::Error error, std::uint64_t busy_job = 0);
        ::motion::trajectory::Reply inspect(const std::stop_token& stop = {});
        struct Reservation
        {
            std::uint64_t id{}, configuration{};
            std::uint8_t axes{};
        };
        ::motion::trajectory::Reply acquire(Reservation reservation, const std::stop_token& stop = {});
        void execute(std::uint64_t id,
                     const ::motion::trajectory::Reply& reservation,
                     std::uint64_t configuration,
                     const ::motion::trajectory::FiniteProgram& program,
                     const std::stop_token& stop = {});
        void cancel(std::uint64_t id);
        void cancelFamily(::motion::Family family);
        void stop(std::uint8_t axes = ALL_AXES, bool abort = false);
        JobInfo wait(std::uint64_t id, std::chrono::milliseconds timeout, const std::stop_token& stop = {});
        void run(const std::stop_token& stop);

      private:
        static constexpr std::size_t MAX_JOBS{ 8 }, HISTORY_CAPACITY{ 32 }, CONTROL_CAPACITY{ 4 },
          COMPLETION_CAPACITY{ CONTROL_CAPACITY + 1 };
        static constexpr std::uint8_t ALL_AXES{ 7 };
        static constexpr auto INPUT_TIMEOUT{ std::chrono::milliseconds{ 200 } },
          RENEW_TIMEOUT{ std::chrono::milliseconds{ 150 } };
        static constexpr auto REQUEST_TIMEOUT{ std::chrono::seconds{ 3 } };
        using ControlResult = std::expected<::motion::MotorResponse, pnm::msg::ServiceError>;
        struct ControlBatch
        {
            std::array<Completion, COMPLETION_CAPACITY> callbacks{};
            std::array<ControlResult, COMPLETION_CAPACITY> results{};
            unsigned count{};
        };
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
            std::optional<std::chrono::steady_clock::time_point> inputDeadline;
        };
        struct ControlCall
        {
            ::motion::MotorRequest request;
            Completion completion;
            bool cancelled{};
            std::uint64_t source{};
            std::optional<pnm::msg::PendingCall<::motion::MotorResponse>> handle;
            pnm::msg::ReadinessRegistration readiness;
        };
        std::array<std::optional<ControlCall>, CONTROL_CAPACITY> m_calls;
        std::optional<ControlCall> m_disable;
        std::optional<pnm::msg::PendingCall<::motion::trajectory::Packet>> m_stopCall;
        pnm::msg::ReadinessRegistration m_stopReady;
        std::uint8_t m_stopAxes{}, m_abortAxes{};
        struct Remote {
            std::uint64_t source{};
            std::chrono::steady_clock::time_point deadline{};
            bool active{};
        };
        static constexpr auto REMOTE_TIMEOUT{ std::chrono::milliseconds{500} };
        std::array<Remote, 2> m_remote;
        std::uint64_t m_nextRemote{1};
        bool m_remotePermission{};
        bool remoteAuthorizedLocked(std::uint64_t source) const;
        void expireInputs();
        void checkRawOwnerLocked(std::uint8_t axes, std::uint64_t source) const;
        std::function<void()> m_observer;
        void serviceControls();
        void serviceCallLocked(std::optional<ControlCall>& /*slot*/, bool urgent, ControlBatch& /*batch*/);
        void finishStopLocked(pnm::msg::PendingCall<::motion::trajectory::Packet>& call);
        void serviceStopLocked();
        void completeRenewalLocked(std::chrono::steady_clock::time_point now);
        static pnm::msg::ActionCallbacks<::motion::MotorProgress, ::motion::MotorResult> motorCallbacks(
          const std::weak_ptr<Job>& weak);
        void shutdownJobs();
        struct History
        {
            std::array<JobInfo, HISTORY_CAPACITY> jobs{};
            unsigned count{};
        };
        static_assert(std::is_trivially_copyable_v<History>);
        struct ReleaseHistory
        {
            void operator()(History* /*history*/) const noexcept;
        };
        static std::unique_ptr<History, ReleaseHistory> reserveHistory();
        std::unique_ptr<History, ReleaseHistory> m_history;
        void retireJobsLocked();
        std::shared_ptr<Job> findLocked(std::uint64_t id) const;
        std::shared_ptr<Job> createLocked(::motion::Family family, std::uint8_t axes, std::uint64_t source);
        ::motion::trajectory::Reply exchange(const ::motion::trajectory::Request& /*value*/,
                                             bool urgent,
                                             const std::stop_token& stop = {});
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
