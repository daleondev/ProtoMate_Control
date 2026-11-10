#include "RobotConfiguration.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace ui::motion
{
    using namespace pnm::units;
    using namespace pnm::units::literals;
    namespace tr = ::motion::trajectory;
    namespace
    {
        constexpr double BOUNDARY_ROUNDOFF_ULPS{ 8 };
        constexpr unsigned BYTE_BITS{ 8 };
        constexpr std::uint64_t BYTE_MASK{ 255 }, FNV_PRIME{ 1099511628211ULL };
        // These are explicit preview/test-fixture values, not physical calibration.
        namespace preview
        {
            constexpr double ROTARY_REDUCTION{ 6.0 }, SECOND_REFERENCE_RAD{ 1.2 }, MINIMUM_BEND_SINE{ .05 };
            constexpr RotaryDynamics ROTARY_DYNAMICS{ .velocity = 5_rpm,
                                                      .acceleration = 120_deg_s2,
                                                      .deceleration = 120_deg_s2,
                                                      .jerk = 600_deg_s3 };
            constexpr LinearDynamics LINEAR_DYNAMICS{ .velocity = 20_mm_s,
                                                      .acceleration = 100_mm_s2,
                                                      .deceleration = 100_mm_s2,
                                                      .jerk = 500_mm_s3 };
            constexpr auto SCREW_LEAD{ 8_mm }, Z_REFERENCE{ 30_mm };
            constexpr auto ARM_LENGTH{ 180_mm }, ARCHIVED_ARM1{ 205_mm }, ARCHIVED_ARM2{ 223.4_mm },
              ARCHIVED_TOOL{ 135_mm };
            constexpr ScaraKinematics::JointLimits JOINT_LIMITS{
                .minimum = { .shoulder = -150_deg, .elbow = 10_deg, .z = 0_mm },
                .maximum = { .shoulder = 150_deg, .elbow = 170_deg, .z = 60_mm }
            };
            constexpr CartesianPosition WORKSPACE_MINIMUM{ -600_mm, -600_mm, 0_mm },
              WORKSPACE_MAXIMUM{ 600_mm, 600_mm, 60_mm };
            constexpr auto FEED{ 20_mm_s };
            constexpr auto PATH_TOLERANCE{ .5_mm };
            constexpr auto HOME_SEEK{ 5_rpm }, HOME_LATCH{ .5_rpm };
            constexpr auto HOME_TIMEOUT{ 30_s };
        }
        void require(bool valid)
        {
            if (!valid) {
                throw std::invalid_argument("invalid robot configuration");
            }
        }
        bool positive(double n) { return std::isfinite(n) && n > 0; }
        double step_boundary(double offset, double scaled) noexcept
        {
            const double value{ offset + scaled };
            const double integer{ std::round(value) };
            // Cancelled affine terms can put an exact integer boundary a few
            // floating-point ulps below that integer. Correct only that numeric
            // roundoff, never round a genuinely fractional soft limit outward.
            const double tolerance{ BOUNDARY_ROUNDOFF_ULPS * std::numeric_limits<double>::epsilon() *
                                    (std::abs(offset) + std::abs(scaled) + 1) };
            return std::abs(value - integer) <= tolerance ? integer : value;
        }
        bool permutation(const std::array<std::uint8_t, 3>& indices)
        {
            auto copy{ indices };
            std::ranges::sort(copy);
            return copy == std::array<std::uint8_t, 3>{ 0, 1, 2 };
        }
        std::array<double, 3> coordinates(CartesianPosition p) { return { p.x.get(), p.y.get(), p.z.get() }; }
    }

    std::optional<RobotConfiguration> board_robot_configuration() { return std::nullopt; }
    namespace
    {
        // Immutable-after-start provider binding for native simulation configuration.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
        ConfigurationProvider simulation_provider{};
    }
    void set_simulation_configuration_provider(ConfigurationProvider provider)
    {
        simulation_provider = std::move(provider);
    }
    std::optional<RobotConfiguration> execution_configuration(const tr::Capabilities& hardware)
    {
        if (simulation_provider) {
            return simulation_provider(hardware);
        }
        return board_robot_configuration();
    }

    RobotConfiguration simulation_configuration(const tr::Capabilities& hardware, bool archived)
    {
        RobotConfiguration c{};
        c.expected_hardware = hardware.hardware_fingerprint;
        for (unsigned i{ 0 }; i < 2; ++i) {
            c.rotary[i] = { .motor_revolutions_per_axis_revolution = preview::ROTARY_REDUCTION,
                            .motor_reference =
                              Angle::create(hardware.reference_steps[i] * hardware.step_radians[i]),
                            .axis_reference = Angle::create(i == 0 ? 0.0 : preview::SECOND_REFERENCE_RAD) };
            c.rotary_dynamics[i] = preview::ROTARY_DYNAMICS;
        }
        c.linear = { .travel_per_output_revolution = preview::SCREW_LEAD,
                     .motor_revolutions_per_output_revolution = 1.0,
                     .motor_reference = Angle::create(hardware.reference_steps[2] * hardware.step_radians[2]),
                     .axis_reference = preview::Z_REFERENCE };
        c.linear_dynamics = preview::LINEAR_DYNAMICS;
        c.geometry.first_arm_length = archived ? preview::ARCHIVED_ARM1 : preview::ARM_LENGTH;
        c.geometry.second_arm_length = archived ? preview::ARCHIVED_ARM2 : preview::ARM_LENGTH;
        c.geometry.tool_offset.x = archived ? preview::ARCHIVED_TOOL : 0_mm;
        c.geometry.minimum_bend_sine = preview::MINIMUM_BEND_SINE;
        c.geometry.limits = preview::JOINT_LIMITS;
        c.workspace_minimum = preview::WORKSPACE_MINIMUM;
        c.workspace_maximum = preview::WORKSPACE_MAXIMUM;
        c.feed = preview::FEED;
        c.path_tolerance = preview::PATH_TOLERANCE;
        c.blend_tolerance = 1_mm;
        c.joint_blend_tolerance = { .shoulder = 1_deg, .elbow = 1_deg, .z = 1_mm };
        c.homing_order = { 2, 0, 1 };
        for (unsigned i{ 0 }; i < c.homing.size(); ++i) {
            c.homing[i] = { .configured = true,
                            .positive = bool(hardware.reference_forward_mask & (1U << i)),
                            .seek = preview::HOME_SEEK,
                            .latch = preview::HOME_LATCH,
                            .timeout = preview::HOME_TIMEOUT };
        }
        return c;
    }

    RobotModel::RobotModel(RobotConfiguration c, tr::Capabilities h, Scope scope)
      : m_configuration{ c }
      , m_hardware{ h }
      , m_rotary{ RotaryAxisConversion(c.rotary[0]), RotaryAxisConversion(c.rotary[1]) }
      , m_linear{ c.linear }
    {
        require(h.version == tr::VERSION && h.tick_ns == tr::TICK_NS && c.expected_hardware != 0 &&
                c.expected_hardware == h.hardware_fingerprint &&
                h.hardware_fingerprint == tr::fingerprint(h));
        require(permutation(c.motor_for_joint) && permutation(c.homing_order) &&
                c.geometry.limits.has_value());
        if (scope == Scope::Robot) {
            m_kinematics.emplace(c.geometry);
            require(positive(c.feed.get()) && positive(c.path_tolerance.get()) &&
                    positive(c.blend_tolerance.get()));
            require(c.work.yaw.isFinite());
        }
        require(h.capacity >= 2 && h.capacity <= tr::IO_SEGMENTS && h.high_ticks > 0 &&
                h.minimum_period_ticks > h.high_ticks && h.direction_setup_ticks > 0 &&
                h.direction_hold_ticks > 0);
        for (const auto& l : h.limits) {
            require(std::isfinite(l.minimum) && std::isfinite(l.maximum) && l.minimum < l.maximum &&
                    l.minimum >= -tr::MAX_POSITION && l.maximum <= tr::MAX_POSITION && positive(l.velocity) &&
                    positive(l.acceleration) && positive(l.deceleration) && positive(l.jerk));
        }
        if (scope == Scope::Robot) {
            for (double v : joints(c.joint_blend_tolerance)) {
                require(positive(v));
            }
            for (double value : coordinates(c.work.origin)) {
                require(std::isfinite(value));
            }
            auto low{ coordinates(c.workspace_minimum) };
            auto high{ coordinates(c.workspace_maximum) };
            for (unsigned i{ 0 }; i < 3; ++i) {
                require(std::isfinite(low[i]) && std::isfinite(high[i]) && low[i] < high[i]);
            }
        }
        for (unsigned i{ 0 }; i < 3; ++i) {
            require(positive(h.step_radians[i]) && std::isfinite(h.reference_steps[i]));
        }
        for (unsigned j{ 0 }; j < m_scale.size(); ++j) {
            initializeJoint(j);
        }
        if (c.neutral_position) {
            const auto q{ joints(*c.neutral_position) };
            const auto limits{ jointLimits() };
            for (unsigned j{ 0 }; j < 3; ++j) {
                require(std::isfinite(q[j]) && q[j] >= limits[j].minimum && q[j] <= limits[j].maximum);
            }
        }
        for (const auto& l : jointLimits()) {
            require(std::isfinite(l.minimum) && std::isfinite(l.maximum) && l.minimum < l.maximum &&
                    positive(l.velocity) && positive(l.acceleration) && positive(l.deceleration) &&
                    positive(l.jerk));
        }
    }

    void RobotModel::initializeJoint(unsigned j)
    {
        const auto& c{ m_configuration };
        const auto& h{ m_hardware };
        const auto m{ c.motor_for_joint[j] };
        if (j < 2) {
            m_offset[j] = m_rotary[j].toMotorPosition(0_rad).get() / h.step_radians[m];
            m_scale[j] = m_rotary[j].toMotorDisplacement(1_rad).get() / h.step_radians[m];
        }
        else {
            m_offset[j] = m_linear.toMotorPosition(0_m).get() / h.step_radians[m];
            m_scale[j] = m_linear.toMotorDisplacement(1_m).get() / h.step_radians[m];
        }
        // IO transmits integer pulse coordinates. The physical homing datum
        // need not be an integer multiple of a microstep: preserve that
        // datum by translating the grid, rather than shifting the joint
        // position (and potentially placing it beyond its soft limit).
        const auto reference{ tr::quantize(h.reference_steps[m]) };
        require(reference.has_value());
        m_offset[j] += double(*reference) - h.reference_steps[m];
        require(std::isfinite(m_offset[j]) && positive(std::abs(m_scale[j])));
        const auto& home{ c.homing[j] };
        if (home.configured) {
            require(positive(home.seek.get()) && positive(home.latch.get()) && home.latch < home.seek &&
                    positive(home.timeout.get()));
        }
    }

    RobotError RobotModel::physicalAdmission() const noexcept
    {
        if (m_configuration.purpose != RobotConfiguration::Purpose::Physical || !m_configuration.calibrated ||
            !std::ranges::all_of(m_configuration.homing, [](const auto& h) { return h.configured; })) {
            return RobotError::Configuration;
        }
        return m_hardware.coordinated_execution ? RobotError::None : RobotError::Unsupported;
    }
    std::array<double, 3> RobotModel::joints(JointPosition p) noexcept
    {
        return { p.shoulder.get(), p.elbow.get(), p.z.get() };
    }
    JointPosition RobotModel::joints(const std::array<double, 3>& p) noexcept
    {
        return { Angle::create(p[0]), Angle::create(p[1]), Distance::create(p[2]) };
    }
    tr::Positions RobotModel::toMotors(JointPosition p) const
    {
        tr::Positions result{};
        const auto q{ joints(p) };
        for (unsigned j{ 0 }; j < 3; ++j) {
            const auto m{ m_configuration.motor_for_joint[j] };
            result[m] = m_offset[j] + (m_scale[j] * q[j]);
            if (!std::isfinite(result[m]) || std::abs(result[m]) > tr::MAX_POSITION) {
                throw std::out_of_range("robot motor coordinate");
            }
        }
        return result;
    }
    JointPosition RobotModel::toJoints(const tr::Positions& p) const
    {
        std::array<double, 3> q{};
        for (unsigned j{ 0 }; j < 3; ++j) {
            const double value{ p[m_configuration.motor_for_joint[j]] };
            if (!std::isfinite(value) || std::abs(value) > tr::MAX_POSITION) {
                throw std::out_of_range("robot motor coordinate");
            }
            q[j] = (value - m_offset[j]) / m_scale[j];
        }
        return joints(q);
    }
    tr::MotorStates RobotModel::toMotors(const std::array<::motion::KinematicState, 3>& q) const
    {
        tr::MotorStates result{};
        for (unsigned j{ 0 }; j < 3; ++j) {
            result[m_configuration.motor_for_joint[j]] = { .position =
                                                             m_offset[j] + (m_scale[j] * q[j].position),
                                                           .velocity = m_scale[j] * q[j].velocity,
                                                           .acceleration = m_scale[j] * q[j].acceleration };
        }
        return result;
    }
    const ScaraKinematics& RobotModel::kinematics() const
    {
        if (!m_kinematics) {
            throw std::logic_error("robot kinematics unavailable: model was configured for axes only");
        }
        return *m_kinematics;
    }
    std::array<tr::Limits, 3> RobotModel::jointLimits() const noexcept
    {
        // The constructor validates limits for both Axes and Robot scopes;
        // m_configuration is private and immutable after construction.
        // NOLINTBEGIN(bugprone-unchecked-optional-access)
        const auto lo{ joints(m_configuration.geometry.limits->minimum) };
        const auto hi{ joints(m_configuration.geometry.limits->maximum) };
        // NOLINTEND(bugprone-unchecked-optional-access)
        std::array<tr::Limits, 3> result{};
        for (unsigned j{ 0 }; j < 2; ++j) {
            const auto& d{ m_configuration.rotary_dynamics[j] };
            result[j] = { .minimum = lo[j],
                          .maximum = hi[j],
                          .velocity = d.velocity.get(),
                          .acceleration = d.acceleration.get(),
                          .deceleration = d.deceleration.get(),
                          .jerk = d.jerk.get() };
        }
        const auto& d{ m_configuration.linear_dynamics };
        result[2] = { .minimum = lo[2],
                      .maximum = hi[2],
                      .velocity = d.velocity.get(),
                      .acceleration = d.acceleration.get(),
                      .deceleration = d.deceleration.get(),
                      .jerk = d.jerk.get() };
        for (unsigned j{ 0 }; j < 3; ++j) {
            auto& l{ result[j] };
            const auto& h{ m_hardware.limits[m_configuration.motor_for_joint[j]] };
            const double scale{ std::abs(m_scale[j]) };
            l.velocity = std::min(l.velocity, h.velocity / scale);
            l.acceleration = std::min(l.acceleration, h.acceleration / scale);
            l.deceleration = std::min(l.deceleration, h.deceleration / scale);
            l.jerk = std::min(l.jerk, h.jerk / scale);
        }
        return result;
    }
    std::array<tr::Limits, 3> RobotModel::motorLimits() const noexcept
    {
        auto result{ m_hardware.limits };
        const auto joints{ jointLimits() };
        for (unsigned j{ 0 }; j < 3; ++j) {
            auto& m{ result[m_configuration.motor_for_joint[j]] };
            const auto& q{ joints[j] };
            const double scale{ std::abs(m_scale[j]) };
            const double a{ step_boundary(m_offset[j], m_scale[j] * q.minimum) };
            const double b{ step_boundary(m_offset[j], m_scale[j] * q.maximum) };
            m.minimum = std::max(m.minimum, std::min(a, b));
            m.maximum = std::min(m.maximum, std::max(a, b));
            m.velocity = q.velocity * scale;
            m.acceleration = q.acceleration * scale;
            m.deceleration = q.deceleration * scale;
            m.jerk = q.jerk * scale;
        }
        return result;
    }
    CartesianPosition RobotModel::vectorFromWork(CartesianPosition p) const noexcept
    {
        const double c{ std::cos(m_configuration.work.yaw.get()) };
        const double s{ std::sin(m_configuration.work.yaw.get()) };
        return { Distance::create((c * p.x.get()) - (s * p.y.get())),
                 Distance::create((s * p.x.get()) + (c * p.y.get())),
                 p.z };
    }
    CartesianPosition RobotModel::fromWork(CartesianPosition p) const noexcept
    {
        const auto v{ vectorFromWork(p) };
        const auto& o{ m_configuration.work.origin };
        return { v.x + o.x, v.y + o.y, v.z + o.z };
    }
    double RobotModel::quantizationBound() const noexcept
    {
        const auto& g{ m_configuration.geometry };
        const double l2{ std::hypot((g.second_arm_length + g.tool_offset.x).get(), g.tool_offset.y.get()) };
        const double xy{ ((g.first_arm_length.get() + l2) / std::abs(m_scale[0])) +
                         (l2 / std::abs(m_scale[1])) };
        return std::hypot(xy, 1 / std::abs(m_scale[2]));
    }
    std::uint64_t RobotModel::fingerprint() const noexcept
    {
        std::uint64_t hash{ m_configuration.expected_hardware };
        const auto add{ [&](double n) {
            auto bits{ std::bit_cast<std::uint64_t>(n) };
            for (unsigned b{ 0 }; b < sizeof(bits); ++b) {
                hash ^= (bits >> (BYTE_BITS * b)) & BYTE_MASK;
                hash *= FNV_PRIME;
            }
        } };
        for (unsigned j{ 0 }; j < 3; ++j) {
            add(m_configuration.motor_for_joint[j]);
            add(m_scale[j]);
            add(m_offset[j]);
        }
        const auto& g{ m_configuration.geometry };
        for (double n : { g.first_arm_length.get(),
                          g.second_arm_length.get(),
                          g.base_yaw.get(),
                          g.tool_yaw.get(),
                          g.minimum_bend_sine }) {
            add(n);
        }
        for (auto p : { g.base_origin,
                        g.tool_offset,
                        m_configuration.workspace_minimum,
                        m_configuration.workspace_maximum,
                        m_configuration.work.origin }) {
            for (double n : coordinates(p)) {
                add(n);
            }
        }
        add(m_configuration.neutral_position.has_value());
        if (m_configuration.neutral_position) {
            for (auto q : joints(*m_configuration.neutral_position)) {
                add(q);
            }
        }
        add(m_configuration.work.yaw.get());
        add(m_configuration.path_tolerance.get());
        add(static_cast<unsigned>(m_configuration.purpose));
        add(m_configuration.calibrated);
        add(m_configuration.feed.get());
        add(m_configuration.blend_tolerance.get());
        for (double v : joints(m_configuration.joint_blend_tolerance)) {
            add(v);
        }
        for (unsigned j{ 0 }; j < 3; ++j) {
            add(m_configuration.homing_order[j]);
            const auto& home{ m_configuration.homing[j] };
            add(home.configured);
            add(home.positive);
            add(home.seek.get());
            add(home.latch.get());
            add(home.timeout.get());
        }
        for (const auto& l : jointLimits()) {
            for (double n : { l.minimum, l.maximum, l.velocity, l.acceleration, l.deceleration, l.jerk }) {
                add(n);
            }
        }
        return hash;
    }
}
