#pragma once

#include "Planner.hpp"
#include "runtime/synchronization/Notification.hpp"
#include <memory>
#include <mutex>
#include <pneumo/messaging.hpp>
#include <stop_token>

namespace ui::motion
{
    // Application-owned planning entry point. CLI submission never performs
    // kinematics or curve subdivision on the input/ICC threads.
    class Robot final
    {
      public:
        enum class Model : std::uint8_t
        {
            Physical,
            Fixture180,
            FixtureArchived
        };
        enum class JobState : std::uint8_t
        {
            Empty,
            Queued,
            Planning,
            Preview,
            Rejected,
            Cancelled,
            Unknown
        };
        struct PreviewRequest
        {
            Move move;
            Model model{ Model::Physical };
            JointPosition start{};
        };
        struct Job
        {
            std::uint64_t id{};
            JobState state{ JobState::Empty };
            RobotError error{ RobotError::None };
            Model model{ Model::Physical };
            PlanSummary plan{};
            JointPosition endpoint{};
            CartesianPosition tcp{};
        };
        explicit Robot(pnm::msg::Bus& bus);
        ~Robot();
        Robot(const Robot&) = delete;
        Robot& operator=(const Robot&) = delete;
        void setConnected(bool connected);
        [[nodiscard]] std::expected<std::uint64_t, RobotError> preview(PreviewRequest request);
        [[nodiscard]] RobotError cancel(std::uint64_t job);
        [[nodiscard]] std::optional<Job> job(std::uint64_t id) const;
        [[nodiscard]] std::array<std::uint64_t, ::motion::trajectory::COMMANDS> jobs() const;
        void run(std::stop_token stop);

      private:
        struct Slot
        {
            PreviewRequest request;
            Job job;
        };
        struct Arena
        {
            Planner::Workspace workspace;
            std::array<Slot, ::motion::trajectory::COMMANDS> slots;
        };
        static_assert(std::is_trivially_copyable_v<Arena>);
        static_assert(sizeof(Arena) <= 256 * 1024);
        struct ReleaseArena
        {
            void operator()(Arena*) const noexcept;
        };
        static std::unique_ptr<Arena, ReleaseArena> reserveArena();
        Job calculate(PreviewRequest request, std::uint64_t id, std::stop_token stop);
        pnm::msg::Service<::motion::trajectory::Packet, ::motion::trajectory::Packet> m_service;
        std::unique_ptr<Arena, ReleaseArena> m_arena;
        mutable std::mutex m_mutex;
        runtime::Notification m_changed;
        std::uint64_t m_next{ 1 };
        bool m_connected{}, m_stopped{};
    };
}
