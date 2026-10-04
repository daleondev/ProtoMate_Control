#include "Robot.hpp"
#include "SynchronizedSequence.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace control
{
    namespace
    {
        template<typename Converter>
        const Converter& conversion(const MotionController::AxisStatus& axis)
        {
            if (!axis.conversion)
                throw std::runtime_error("robot requires configured shoulder, elbow and Z mechanics");
            const auto* value{ std::get_if<Converter>(&*axis.conversion) };
            if (!value)
                throw std::logic_error("robot requires rotary shoulder/elbow and linear Z axes");
            return *value;
        }

        ScaraKinematics::JointPosition joints(const MotionController::Status& status)
        {
            return {
                conversion<RotaryAxisConversion>(status.axes[0]).toAxisPosition(status.axes[0].position),
                conversion<RotaryAxisConversion>(status.axes[1]).toAxisPosition(status.axes[1].position),
                conversion<LinearAxisConversion>(status.axes[2]).toAxisPosition(status.axes[2].position)
            };
        }

        template<typename T>
        T checked(ScaraKinematics::Result<T> result)
        {
            if (result)
                return *result;
            using enum ScaraKinematics::Error;
            switch (result.error()) {
                case InvalidInput:
                    throw std::invalid_argument("invalid robot coordinates");
                case Unreachable:
                    throw std::invalid_argument("robot target is unreachable");
                case JointLimitExceeded:
                    throw std::invalid_argument("robot joint limit exceeded");
                case Singularity:
                    throw std::invalid_argument(
                      "robot singularity; select an explicit elbow branch when leaving a singular pose");
                case NumericOverflow:
                    throw std::overflow_error("robot coordinate overflow");
            }
            std::unreachable();
        }

        void validate(const Robot::MoveOptions& options)
        {
            if (!std::isfinite(options.speed) || options.speed <= 0.0 || options.speed > 1.0)
                throw std::invalid_argument("robot speed must be greater than 0 and at most 100 percent");
        }
    }

    Robot::Robot(std::shared_ptr<MotionController> controller, ScaraKinematics::Config geometry)
      : Robot{ std::move(controller), geometry, { 60_deg / 1_s, 60_deg / 1_s, 20_mm / 1_s } }
    {
    }

    Robot::Robot(std::shared_ptr<MotionController> controller,
                 ScaraKinematics::Config geometry,
                 ScaraKinematics::JointVelocity maximum_velocity)
      : m_controller{ std::move(controller) }
      , m_kinematics{ geometry }
      , m_maximumVelocity{ maximum_velocity }
    {
        if (!m_controller)
            throw std::invalid_argument("robot requires a motion controller");
        if (!maximum_velocity.shoulder.isFinite() || !maximum_velocity.elbow.isFinite() ||
            !maximum_velocity.z.isFinite() || maximum_velocity.shoulder <= 0_rpm ||
            maximum_velocity.elbow <= 0_rpm || maximum_velocity.z <= 0_mm / 1_s)
            throw std::invalid_argument("robot joint speed limits must be finite and positive");
    }

    Robot::Motion Robot::moveAbs(ScaraKinematics::CartesianPosition target, MoveOptions options)
    {
        validate(options);
        return m_controller->coordinate([&](const auto& state, const auto& defaults) {
            return plan(state,
                        defaults,
                        checked(m_kinematics.inverse(target, joints(state), options.branch)),
                        options.speed);
        }, options.timeout);
    }

    Robot::Motion Robot::moveRel(ScaraKinematics::CartesianPosition distance, MoveOptions options)
    {
        validate(options);
        return m_controller->coordinate([&](const auto& state, const auto& defaults) {
            const auto start{ joints(state) };
            auto target{ checked(m_kinematics.forward(start)).position };
            target.x += distance.x;
            target.y += distance.y;
            target.z += distance.z;
            return plan(
              state, defaults, checked(m_kinematics.inverse(target, start, options.branch)), options.speed);
        }, options.timeout);
    }

    Robot::Motion Robot::moveJoints(ScaraKinematics::JointPosition target, MoveOptions options)
    {
        validate(options);
        if (options.branch)
            throw std::invalid_argument("joint moves specify the elbow directly; no branch option applies");
        return m_controller->coordinate([&](const auto& state, const auto& defaults) {
            return plan(state, defaults, target, options.speed);
        }, options.timeout);
    }

    Robot::Motion Robot::reference(pnm::units::Time timeout) { return m_controller->referenceAll(timeout); }
    void Robot::enable() { m_controller->enable(); }
    void Robot::disable() { m_controller->disable(); }
    void Robot::stop() { m_controller->stop(); }
    void Robot::reset() { m_controller->reset(); }
    std::vector<Robot::Motion> Robot::motions() { return m_controller->groupMotions(); }

    MotionController::CoordinatedPlan Robot::plan(const MotionController::Status& origin,
                                                  const std::array<StepperMotor::MotionDefaults, 3>& defaults,
                                                  ScaraKinematics::JointPosition target,
                                                  double speed) const
    {
        // Rectangular joint limits hold along the whole joint-linear segment
        // when both endpoints fit. This is not collision or tool-path planning.
        const auto checkLimits = [&](auto position) { checked(m_kinematics.checkJointLimits(position)); };
        checkLimits(joints(origin));
        checkLimits(target);
        const auto& shoulder{ conversion<RotaryAxisConversion>(origin.axes[0]) };
        const auto& elbow{ conversion<RotaryAxisConversion>(origin.axes[1]) };
        const auto& z{ conversion<LinearAxisConversion>(origin.axes[2]) };
        std::array motor_target{ shoulder.toMotorPosition(target.shoulder),
                                 elbow.toMotorPosition(target.elbow),
                                 z.toMotorPosition(target.z) };
        const std::array max_speed{ shoulder.toMotorSpeed(m_maximumVelocity.shoulder),
                                    elbow.toMotorSpeed(m_maximumVelocity.elbow),
                                    z.toMotorSpeed(m_maximumVelocity.z) };
        std::array<hal::step::PulseCount, 3> counts{};
        MotionController::CoordinatedPlan result{};
        const auto infinity{ std::numeric_limits<double>::infinity() };
        motion::Profile::Limits limits{ infinity, infinity, infinity, infinity };
        bool moving{};
        for (std::size_t i{}; i < counts.size(); ++i) {
            const auto config{ m_controller->motorConfiguration(static_cast<MotionController::MotorId>(i)) };
            const auto step{ config.full_step_angle / static_cast<double>(config.microsteps) };
            const auto delta{ motor_target[i] - origin.axes[i].position };
            const auto pulses{ std::round(std::abs(delta / step)) };
            if (!std::isfinite(pulses) || pulses >= 0x1p52)
                throw std::invalid_argument("robot displacement is too large");
            counts[i] = static_cast<hal::step::PulseCount>(pulses);
            result.forward[i] = delta.get() >= 0.0;
            const auto distance{ step.get() * pulses };
            motor_target[i] =
              origin.axes[i].position + pnm::units::Angle::create(result.forward[i] ? distance : -distance);
            if (!counts[i])
                continue;
            moving = true;
            // Leave rounding margin below the hardware's 100 kstep/s ceiling.
            limits.velocity =
              std::min({ limits.velocity, max_speed[i].get() * speed / distance, 99999.0 / pulses });
            limits.acceleration = std::min(limits.acceleration, defaults[i].acceleration.get() / distance);
            limits.deceleration = std::min(limits.deceleration, defaults[i].deceleration.get() / distance);
            if (defaults[i].jerk.get() > 0.0)
                limits.jerk = std::min(limits.jerk, defaults[i].jerk.get() / distance);
        }
        checkLimits(ScaraKinematics::JointPosition{ shoulder.toAxisPosition(motor_target[0]),
                                                    elbow.toAxisPosition(motor_target[1]),
                                                    z.toAxisPosition(motor_target[2]) });
        if (!moving)
            return result;
        if (!std::isfinite(limits.jerk))
            limits.jerk = 0.0;
        const auto profile{ motion::Profile::create(1.0, limits) };
        if (!profile)
            throw std::invalid_argument("robot motion profile cannot be generated");
        result.duration = pnm::units::Time::create(profile->duration()) + 0.010005_s;
        for (std::size_t i{}; i < counts.size(); ++i) {
            if (!counts[i])
                continue;
            auto sequence{ std::make_shared<motion::SynchronizedSequence>(*profile, counts[i]) };
            result.delays[i] = sequence->firstDelay() + std::chrono::milliseconds{ 10 };
            result.sequences[i] = std::move(sequence);
        }
        return result;
    }

    Robot::Status Robot::status() const
    {
        auto motors{ m_controller->status() };
        const auto& shoulder{ conversion<RotaryAxisConversion>(motors.axes[0]) };
        const auto& elbow{ conversion<RotaryAxisConversion>(motors.axes[1]) };
        const auto& z{ conversion<LinearAxisConversion>(motors.axes[2]) };
        const auto state = [&](bool actual) {
            const auto position = [&](std::size_t index) {
                const auto& axis{ motors.axes[index] };
                return actual ? *axis.actual_position : axis.position;
            };
            const auto velocity = [&](std::size_t index) {
                const auto& axis{ motors.axes[index] };
                return actual ? *axis.actual_velocity : axis.velocity;
            };
            const ScaraKinematics::JointPosition joints{ shoulder.toAxisPosition(position(0)),
                                                         elbow.toAxisPosition(position(1)),
                                                         z.toAxisPosition(position(2)) };
            const ScaraKinematics::JointVelocity speeds{ shoulder.toAxisVelocity(velocity(0)),
                                                         elbow.toAxisVelocity(velocity(1)),
                                                         z.toAxisVelocity(velocity(2)) };
            return State{
                joints, speeds, m_kinematics.forward(joints), m_kinematics.forwardVelocity(joints, speeds)
            };
        };
        auto commanded{ state(false) };
        auto actual = [&]() -> std::expected<State, FeedbackError> {
            for (const auto& axis : motors.axes) {
                if (!axis.actual_position)
                    return std::unexpected(FeedbackError{ axis.motor, axis.actual_position.error() });
                if (!axis.actual_velocity)
                    return std::unexpected(FeedbackError{ axis.motor, axis.actual_velocity.error() });
            }
            return state(true);
        }();
        const bool referenced{ std::ranges::all_of(motors.axes,
                                                   [](const auto& axis) { return axis.referenced; }) };
        return { std::move(motors), std::move(commanded), std::move(actual), referenced };
    }
}
