#pragma once
#include <source_location>

#include "ProfileSchedule.hpp"
#include "common/motion/OperatingEnvelope.hpp"
#include <functional>
#include <memory_resource>

namespace io::motion
{
    // Single owning thread. Commands are finite, monotone per axis, and begin
    // and end at rest. The controller serializes admission with motor faults
    // and independent moves. No hardware enable is changed here.
    class Executor final
    {
      public:
        Executor(Executor&&) = delete;
        auto operator=(Executor&&) -> Executor& = delete;

        // Initial production ceiling; numerical/HAL stress tests may use 25 kHz.
        static constexpr std::uint32_t MAX_PRODUCTION_RATE{ ::motion::operating::STEP_RATE };
        static constexpr std::uint32_t DMA_HALF_PULSES{ 64 };
        static constexpr std::uint32_t HORIZON{ ::motion::operating::COMMIT_TICKS };
        static constexpr std::uint32_t RENEW_PERIOD{ ::motion::operating::CHECKPOINT_TICKS };
        static constexpr std::uint32_t LEASE{ ::motion::operating::LEASE_TICKS };
        enum class State : std::uint8_t
        {
            Idle,
            Prepared,
            Running,
            Braking,
            Completed,
            Stopped,
            Faulted
        };
        enum class Reason : std::uint8_t
        {
            None,
            Cancelled,
            LeaseExpired,
            RenewalLate,
            Hardware,
            ScheduleRejected,
            SpliceRejected
        };
        struct Axis
        {
            std::optional<::motion::Profile::Description> profile;
            ::motion::Profile::Limits limits{};
            bool forward{};
            ::motion::ProgressPath path{};
        };
        struct Command
        {
            std::uint64_t id{};
            std::array<Axis, 3> axes{};
            // When present, every participating profile is a positive scaling
            // of this common progress law. Its braking law is solved once.
            std::optional<::motion::Profile::Description> progress{};
            ::motion::Profile::Limits progress_limits{};
            bool rolling{};
        };
        struct Diagnostic
        {
            ScheduleFailure failure{};
            std::uint8_t axis{ 3 }; // 0..2: axis; 3: common progress.
            std::uint64_t cut{};
            ::motion::Profile::State state{};
            ::motion::Profile::Limits limits{};
            double remaining{}, brake_distance{};
        };
        struct Status
        {
            State state{};
            Reason reason{};
            std::uint64_t id{};
            std::array<std::uint64_t, 3> pulses{};
            std::uint64_t elapsed{}, stopping_at{};
            std::uint32_t renewals{}, rejected_renewals{};
            bool counts_exact{ true };
            std::uint32_t maximum_build_us{}, maximum_splice_us{};
            int last_error{};
            Diagnostic diagnostic{};
        };
        using Prepare = std::function<bool(unsigned, bool, std::shared_ptr<const hal::step::Sequence>)>;
        Executor(std::shared_ptr<hal::IStepGenerator> generator,
                 Prepare prepare,
                 std::pmr::memory_resource* memory = std::pmr::get_default_resource());
        ~Executor();
        Executor(const Executor&) = delete;
        Executor& operator=(const Executor&) = delete;
        // Preparation is explicitly outside the short final admission step.
        // It validates all profiles and constructs the initial stopping path.
        hal::util::Result<> prepare(const Command& command, bool bind_outputs = true);
        // Bind a precompiled program only after final admission under Controller.
        hal::util::Result<> bind();
        hal::util::Result<> start(std::uint64_t id);
        hal::util::Result<> submit(const Command& command);
        bool reserved() const noexcept { return active() || m_status.state == State::Prepared; }
        // Both command ID and lease are required; old messages never revive a
        // stopped command. Call service at least every millisecond while active.
        bool renew(std::uint64_t id);
        bool stop(std::uint64_t id);
        void abort(unsigned site = std::source_location::current()
                                     .line()) noexcept; // Immediate local fault response, not a brake.
        bool recover(); // Explicit stopped-only recovery after HAL reinitialization.
        void service();
        Status status() const noexcept { return m_status; }
        bool active() const noexcept
        {
            return m_status.state == State::Running || m_status.state == State::Braking;
        }

      private:
        struct Slot
        {
            std::optional<ProfileSchedule> schedule;
        };
        struct BuildRequest
        {
            unsigned bank{};
            std::uint64_t cut{};
        };
        bool buildAxis(unsigned i,
                       BuildRequest request,
                       const std::optional<::motion::Profile>& shared_brake);
        bool build(unsigned bank, std::uint64_t cut);
        std::optional<bool> finishAxes(const hal::step::Status& hw);
        void replaceFuture(std::uint64_t elapsed);
        bool failed(ScheduleFailure failure,
                    std::uint64_t cut,
                    unsigned axis = 3,
                    double brake_distance = -1) noexcept;
        std::optional<std::uint64_t> elapsed(std::uint32_t tick) noexcept;
        bool prepareProfiles(const Command& command);
        std::array<std::shared_ptr<const hal::step::TimedSequence>, 3> sequences(unsigned bank) const;
        void brake(Reason reason) noexcept;
        void releaseSchedules() noexcept;
        std::shared_ptr<hal::IStepGenerator> m_generator;
        std::array<std::shared_ptr<hal::IStepOutput>, 3> m_outputs;
        std::array<bool, 3> m_completed{};
        Prepare m_prepare;
        std::pmr::memory_resource* m_memory;
        std::array<ProfileSchedule::Source, 3> m_nominal;
        std::array<std::array<std::shared_ptr<Slot>, 3>, 2> m_banks;
        std::array<::motion::Profile::Limits, 3> m_limits{};
        std::optional<::motion::Profile> m_progress;
        ::motion::Profile::Limits m_progressLimits{};
        std::uint8_t m_mask{};
        Status m_status;
        std::uint64_t m_lastId{};
        std::uint32_t m_origin{}, m_lastTick{};
        std::uint64_t m_next{}, m_lease{}, m_elapsed{};
        bool m_clockStarted{}, m_rolling{};
        unsigned m_bank{};
        bool m_final{}, m_outputsBound{};
        std::array<bool, 3> m_forward{};
    };
}
