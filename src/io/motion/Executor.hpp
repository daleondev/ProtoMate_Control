#pragma once

#include "ProfileSchedule.hpp"
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
        // Initial production ceiling; numerical/HAL stress tests may use 25 kHz.
        static constexpr std::uint32_t MAX_PRODUCTION_RATE = 12'500;
        static constexpr std::uint32_t DMA_HALF_PULSES = 32;
        static constexpr std::uint32_t HORIZON = 200'000;     // 20 ms
        // Rebuilding faster consumes the M4 time needed to dispatch ICC during
        // three-axis cruise. The 20 ms frontier retains the same stop bound.
        static constexpr std::uint32_t RENEW_PERIOD = 70'000; // 7 ms
        static constexpr std::uint32_t LEASE = 200'000;       // 20 ms
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
            Hardware
        };
        struct Axis
        {
            std::optional<::motion::Profile::Description> profile;
            ::motion::Profile::Limits limits{};
            bool forward{};
        };
        struct Command
        {
            std::uint64_t id{};
            std::array<Axis, 3> axes{};
        };
        struct Status
        {
            State state{};
            Reason reason{};
            std::uint64_t id{};
            std::array<std::uint64_t, 3> pulses{};
            std::uint32_t elapsed{}, stopping_at{}, renewals{}, rejected_renewals{};
            bool counts_exact{ true };
            std::uint32_t maximum_build_us{}, maximum_splice_us{};
            int last_error{};
        };
        using Prepare = std::function<bool(unsigned, bool, std::shared_ptr<const hal::step::Sequence>)>;
        Executor(std::shared_ptr<hal::IStepGenerator> generator, Prepare prepare,
                 std::pmr::memory_resource* memory = std::pmr::get_default_resource());
        ~Executor();
        Executor(const Executor&) = delete;
        Executor& operator=(const Executor&) = delete;
        // Preparation is explicitly outside the short final admission step.
        // It validates all profiles and constructs the initial stopping path.
        hal::util::Result<> prepare(const Command& command);
        hal::util::Result<> start(std::uint64_t id);
        hal::util::Result<> submit(const Command& command);
        bool reserved() const noexcept { return active() || m_status.state == State::Prepared; }
        // Both command ID and lease are required; old messages never revive a
        // stopped command. Call service at least every millisecond while active.
        bool renew(std::uint64_t id);
        bool stop(std::uint64_t id);
        void abort() noexcept; // Immediate local fault response, not a brake.
        bool recover();        // Explicit stopped-only recovery after HAL reinitialization.
        void service();
        Status status() const noexcept { return m_status; }
        bool active() const noexcept
        {
            return m_status.state == State::Running || m_status.state == State::Braking;
        }

      private:
        struct Bank
        {
            std::array<std::optional<ProfileSchedule>, 3> schedules;
        };
        bool build(unsigned bank, std::uint32_t cut);
        std::array<std::shared_ptr<const hal::step::TimedSequence>, 3> sequences(unsigned bank) const;
        void brake(Reason reason) noexcept;
        void releaseSchedules() noexcept;
        std::shared_ptr<hal::IStepGenerator> m_generator;
        std::array<std::shared_ptr<hal::IStepOutput>, 3> m_outputs;
        std::array<bool, 3> m_completed{};
        Prepare m_prepare;
        std::pmr::memory_resource* m_memory;
        std::array<ProfileSchedule::Source, 3> m_nominal;
        std::array<std::shared_ptr<Bank>, 2> m_banks;
        std::array<::motion::Profile::Limits, 3> m_limits{};
        Status m_status;
        std::uint64_t m_lastId{};
        std::uint32_t m_origin{}, m_next{}, m_lease{};
        unsigned m_bank{};
        bool m_final{};
    };
}
