#pragma once

#include "Client.hpp"
#include "Planner.hpp"
#include "runtime/synchronization/Notification.hpp"
#include <memory>
#include <mutex>
#include <pneumo/messaging.hpp>
#include <stop_token>

namespace ui::motion
{
    template<class Position>
    class Axis;
    // Application-owned planning entry point. CLI submission never performs
    // kinematics or curve subdivision on the input/ICC threads.
    class Robot final
    {
      public:
        Robot(Robot&&) = delete;
        auto operator=(Robot&&) -> Robot& = delete;

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
        Robot(pnm::msg::Bus& bus, Client& client);
        struct Dynamics
        {
            static constexpr double DEFAULT_SPEED{ .25 };
            double speed{ DEFAULT_SPEED }, acceleration{ 1 }, deceleration{ 1 }, jerk{ 1 };
        };
        struct ExecutionRequest
        {
            enum class Kind : std::uint8_t
            {
                Finite,
                Home,
                Jog,
                CartesianJog
            };
            ::motion::Family family{ ::motion::Family::Robot };
            Kind kind{};
            std::uint8_t axes{ ::motion::ALL_AXES_MASK };
            Move move;
            bool relative{};
            Dynamics dynamics;
            // Axis values are SI joint units (rad/m, rad/s or m/s, etc.).
            double distance{}, speed{}, acceleration{}, deceleration{}, jerk{}, timeout{};
            double duration{ 1 };
            bool continuous{};
            std::array<double, 3> direction{};
        };
        std::uint64_t submit(ExecutionRequest request);
        RobotModel model(const std::stop_token& stop = {},
                         RobotModel::Scope scope = RobotModel::Scope::Robot);
        void requestPresentationModel(std::uint64_t boot, std::uint64_t configuration);
        std::optional<RobotModel> presentationModel() const;
        Axis<pnm::units::Angle> shoulder();
        Axis<pnm::units::Angle> elbow();
        Axis<pnm::units::Distance> z();
        std::uint64_t moveRel(JointPosition q, Dynamics dynamics, pnm::units::Time timeout = {});
        std::uint64_t moveAbs(JointPosition q, Dynamics dynamics, pnm::units::Time timeout = {});
        std::uint64_t moveRel(CartesianPosition p, Dynamics dynamics, pnm::units::Time timeout = {});
        std::uint64_t moveAbs(CartesianPosition position, Dynamics dynamics, pnm::units::Time timeout = {});
        std::uint64_t home();
        void stop();
        void stopAxis(unsigned joint);

        ~Robot();
        Robot(const Robot&) = delete;
        Robot& operator=(const Robot&) = delete;
        void setConnected(bool connected);
        [[nodiscard]] std::expected<std::uint64_t, RobotError> preview(PreviewRequest request);
        [[nodiscard]] RobotError cancel(std::uint64_t id);
        [[nodiscard]] std::optional<Job> job(std::uint64_t id) const;
        [[nodiscard]] std::array<std::uint64_t, ::motion::trajectory::COMMANDS> jobs() const;
        void run(const std::stop_token& stop);

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
            struct Pending
            {
                ExecutionRequest request;
                std::uint64_t id{};
            };
            std::array<Pending, ::motion::MOTOR_JOB_CAPACITY> pending{};
        };
        static_assert(std::is_trivially_copyable_v<Arena>);
        static_assert(sizeof(Arena) <= Planner::WORKSPACE_BUDGET);
        struct ReleaseArena
        {
            void operator()(Arena* /*arena*/) const noexcept;
        };
        static std::unique_ptr<Arena, ReleaseArena> reserveArena();
        Job calculate(PreviewRequest request, std::uint64_t id, const std::stop_token& stop);
        struct ExecutionContext
        {
            std::reference_wrapper<const RobotModel> mechanics;
            std::reference_wrapper<const ::motion::trajectory::Reply> snapshot;
            std::uint64_t job;
            std::reference_wrapper<const std::stop_token> stop;
        };
        void execute(ExecutionRequest request, std::uint64_t id, const std::stop_token& stop);
        void executeHome(const ExecutionRequest& /*request*/, const ExecutionContext& /*context*/);
        void executeCartesianJog(const ExecutionRequest& /*request*/, const ExecutionContext& /*context*/);
        std::expected<::motion::trajectory::FiniteProgram, ::motion::trajectory::Error> compileAxis(
          ExecutionRequest /*request*/,
          const ExecutionContext& /*context*/,
          JointPosition start);
        std::expected<::motion::trajectory::FiniteProgram, ::motion::trajectory::Error> compileRobot(
          const ExecutionRequest& /*request*/,
          const ExecutionContext& /*context*/,
          JointPosition start);
        bool executePending(const std::stop_token& stop);
        Client* m_client{};
        pnm::msg::Service<::motion::trajectory::Packet, ::motion::trajectory::Packet> m_service;
        std::unique_ptr<Arena, ReleaseArena> m_arena;
        mutable std::mutex m_mutex;
        std::array<std::uint8_t, 3> m_motorForJoint{ 0, 1, 2 };
        runtime::Notification m_changed;
        std::uint64_t m_next{ 1 };
        // Cancellation state stays in internal RAM, outside the POD SDRAM arena.
        std::uint64_t m_activeJob{};
        std::stop_source m_activeStop{ std::nostopstate };
        bool m_connected{}, m_stopped{};
        std::optional<RobotModel> m_presentationModel;
        std::uint64_t m_modelBoot{}, m_modelConfiguration{};
        bool m_modelRequested{};
        void refreshPresentationModel(std::stop_token stop);
    };
}
