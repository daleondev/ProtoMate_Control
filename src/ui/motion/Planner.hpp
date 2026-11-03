#pragma once

#include "RobotConfiguration.hpp"
#include "common/motion/Profile.hpp"
#include <span>

namespace ui::motion
{
    enum class MoveKind : std::uint8_t { Joints, Point, Linear, JointCurve, CartesianCurve };
    enum class CoordinateFrame : std::uint8_t { Base, Work };
    struct Move
    {
        MoveKind kind{MoveKind::Joints};
        CoordinateFrame frame{CoordinateFrame::Base};
        JointPosition joints{};
        CartesianPosition target{};
        double speed{1};
        pnm::units::Velocity feed{}; // Zero selects configured feed.
        pnm::units::Distance blend{}; // Zero means exact stop.
        std::array<::motion::Polynomial,3> curve{}; // Internal, SI joint/base coordinates.
        std::uint64_t job{};
    };
    struct PathState { double position{}, velocity{}, acceleration{}; };
    struct PlanSummary
    {
        std::size_t segments{};
        std::uint64_t ticks{};
        double duration{}, quantization_bound{}, path_error_bound{}, final_progress{};
        std::array<double,3> peak_velocity{}, peak_acceleration{}, peak_jerk{};
        ::motion::trajectory::Counts endpoint{};
        ScaraKinematics::ElbowBranch branch{};
    };

    class Planner final
    {
      public:
        struct Piece
        {
            double begin{}, end{};
            std::array<::motion::Polynomial,3> joints{};
        };
        // POD arenas may live in UI SDRAM. No mutex, allocator or runtime object
        // is placed there. The caller owns the arena and serializes its use.
        struct Workspace
        {
            std::array<Piece,128> pieces{};
            std::array<::motion::trajectory::Segment,::motion::trajectory::UI_SEGMENTS> segments{};
        };
        Planner(const RobotModel& model, Workspace& workspace);
        [[nodiscard]] RobotError prepare(const Move& move, const ::motion::trajectory::Counts& start);
        [[nodiscard]] std::expected<PlanSummary,RobotError> compile(PathState start = {}, double override = 1,
                                                                 bool stopping = false);
        [[nodiscard]] std::span<const ::motion::trajectory::Segment> segments() const noexcept;
        [[nodiscard]] PathState pathAt(double seconds) const noexcept;
        [[nodiscard]] const Move& command() const noexcept { return m_move; }
        // Validate a motor-space segment continuously, including workspace and
        // singularity constraints. Optional reference is a TCP curve in SI units.
        [[nodiscard]] RobotError validatePath(const ::motion::trajectory::Segment& segment,
                                              const std::array<::motion::Polynomial,3>* reference = nullptr) const;
      private:
        using JointState = std::array<::motion::KinematicState,3>;
        [[nodiscard]] std::expected<JointState,RobotError> inverseAt(double s) const;
        [[nodiscard]] RobotError build(double from, double to, unsigned depth);
        [[nodiscard]] RobotError certify(const std::array<::motion::Polynomial,3>& q,
                                         const std::array<::motion::Polynomial,3>* desired,
                                         double from, double to, unsigned depth, unsigned& budget) const;
        [[nodiscard]] JointState jointAt(double s, double velocity, double acceleration) const;
        [[nodiscard]] RobotError compileInterval(std::uint64_t from, std::uint64_t to, unsigned depth);
        [[nodiscard]] RobotError certifyReference(const std::array<::motion::Polynomial,3>& reference,
            double start, double duration, double from, double to, unsigned depth, unsigned& budget) const;
        [[nodiscard]] ::motion::Profile::Limits scalarLimits(double speed) const;
        const RobotModel* m_model;
        Workspace* m_workspace;
        Move m_move;
        JointPosition m_seed{};
        ScaraKinematics::ElbowBranch m_branch{};
        std::array<::motion::Polynomial,3> m_cartesian{};
        std::size_t m_pieces{},m_segments{};
        bool m_cartesianPath{},m_prepared{};
        double m_offset{},m_pathDistance{},m_finalProgress{};
        ::motion::Profile m_profile;
        std::uint64_t m_durationTicks{};
    };
    static_assert(std::is_trivially_copyable_v<Planner::Workspace>);
    static_assert(sizeof(Planner::Workspace)*2 <= 256*1024);
}
