#include "Robot.hpp"

#include <algorithm>
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
    }

    Robot::Robot(std::shared_ptr<MotionController> controller, ScaraKinematics::Config geometry)
      : m_controller{ std::move(controller) }
      , m_kinematics{ geometry }
    {
        if (!m_controller)
            throw std::invalid_argument("robot requires a motion controller");
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
