#include "AxisConversion.hpp"

#include <cmath>
#include <stdexcept>

namespace
{
    using namespace pnm::units;
    using namespace pnm::units::literals;

    template<IsQuantity Quantity>
    Quantity finite_input(Quantity value)
    {
        if (!value.isFinite()) {
            throw std::invalid_argument("axis conversion requires finite coordinates and rates");
        }
        return value;
    }

    template<IsQuantity Quantity>
    Quantity finite_result(Quantity value)
    {
        if (!value.isFinite()) {
            throw std::overflow_error("axis conversion overflow");
        }
        return value;
    }

    template<IsQuantity Quantity>
    Quantity speed_input(Quantity value)
    {
        if (finite_input(value).get() < 0.0) {
            throw std::invalid_argument("axis speed/profile limit must be a nonnegative magnitude");
        }
        return value;
    }

    void positive_ratio(double value)
    {
        if (!std::isfinite(value) || value <= 0.0) {
            throw std::invalid_argument("axis transmission ratio must be positive and finite");
        }
    }

    double direction_sign(AxisDirection direction)
    {
        switch (direction) {
            case AxisDirection::SameAsMotor:
                return 1.0;
            case AxisDirection::OppositeToMotor:
                return -1.0;
        }
        throw std::invalid_argument("invalid axis direction");
    }

    double rotary_motor_per_axis(const RotaryAxisConversion::Config& config)
    {
        positive_ratio(config.motor_revolutions_per_axis_revolution);
        finite_input(config.motor_reference);
        finite_input(config.axis_reference);
        return direction_sign(config.direction) * config.motor_revolutions_per_axis_revolution;
    }

    double linear_axis_per_motor(const LinearAxisConversion::Config& config)
    {
        positive_ratio(config.motor_revolutions_per_output_revolution);
        positive_ratio(config.travel_per_output_revolution.get());
        finite_input(config.motor_reference);
        finite_input(config.axis_reference);
        const auto factor = direction_sign(config.direction) * config.travel_per_output_revolution.get() /
                            config.motor_revolutions_per_output_revolution / (1_rev).get();
        positive_ratio(std::abs(factor));
        return factor;
    }

    // Conversion factors use quantity base units (rad, m, rad/s and m/s).
    template<IsQuantity Result, IsQuantity Input>
    Result convert(Input value, double factor)
    {
        return finite_result(Result::create(finite_input(value).get() * factor));
    }
}

RotaryAxisConversion::RotaryAxisConversion(Config config)
  : m_config{ config }
  , m_motorPerAxis{ rotary_motor_per_axis(config) }
  , m_axisPerMotor{ 1.0 / m_motorPerAxis }
{
    positive_ratio(std::abs(m_axisPerMotor));
}

pnm::units::Angle RotaryAxisConversion::toAxisPosition(pnm::units::Angle motor_position) const
{
    const auto displacement{ finite_result(finite_input(motor_position) - m_config.motor_reference) };
    return finite_result(m_config.axis_reference + toAxisDisplacement(displacement));
}

pnm::units::Angle RotaryAxisConversion::toMotorPosition(pnm::units::Angle axis_position) const
{
    const auto displacement{ finite_result(finite_input(axis_position) - m_config.axis_reference) };
    return finite_result(m_config.motor_reference + toMotorDisplacement(displacement));
}

pnm::units::Angle RotaryAxisConversion::toAxisDisplacement(pnm::units::Angle motor_displacement) const
{
    return convert<pnm::units::Angle>(motor_displacement, m_axisPerMotor);
}

pnm::units::Angle RotaryAxisConversion::toMotorDisplacement(pnm::units::Angle axis_displacement) const
{
    return convert<pnm::units::Angle>(axis_displacement, m_motorPerAxis);
}

pnm::units::AngularVelocity RotaryAxisConversion::toAxisVelocity(
  pnm::units::AngularVelocity motor_velocity) const
{
    return convert<pnm::units::AngularVelocity>(motor_velocity, m_axisPerMotor);
}

pnm::units::AngularVelocity RotaryAxisConversion::toMotorVelocity(
  pnm::units::AngularVelocity axis_velocity) const
{
    return convert<pnm::units::AngularVelocity>(axis_velocity, m_motorPerAxis);
}

pnm::units::AngularVelocity RotaryAxisConversion::toAxisSpeed(pnm::units::AngularVelocity motor_speed) const
{
    return convert<pnm::units::AngularVelocity>(speed_input(motor_speed), std::abs(m_axisPerMotor));
}

pnm::units::AngularVelocity RotaryAxisConversion::toMotorSpeed(pnm::units::AngularVelocity axis_speed) const
{
    return convert<pnm::units::AngularVelocity>(speed_input(axis_speed), std::abs(m_motorPerAxis));
}

pnm::units::AngularAcceleration RotaryAxisConversion::toAxisAcceleration(
  pnm::units::AngularAcceleration motor) const
{
    return convert<pnm::units::AngularAcceleration>(speed_input(motor), std::abs(m_axisPerMotor));
}

pnm::units::AngularAcceleration RotaryAxisConversion::toMotorAcceleration(
  pnm::units::AngularAcceleration axis) const
{
    return convert<pnm::units::AngularAcceleration>(speed_input(axis), std::abs(m_motorPerAxis));
}

pnm::units::AngularJerk RotaryAxisConversion::toAxisJerk(pnm::units::AngularJerk motor) const
{
    return convert<pnm::units::AngularJerk>(speed_input(motor), std::abs(m_axisPerMotor));
}

pnm::units::AngularJerk RotaryAxisConversion::toMotorJerk(pnm::units::AngularJerk axis) const
{
    return convert<pnm::units::AngularJerk>(speed_input(axis), std::abs(m_motorPerAxis));
}

LinearAxisConversion::LinearAxisConversion(Config config)
  : m_config{ config }
  , m_axisPerMotor{ linear_axis_per_motor(config) }
  , m_motorPerAxis{ 1.0 / m_axisPerMotor }
{
    positive_ratio(std::abs(m_motorPerAxis));
}

pnm::units::Distance LinearAxisConversion::toAxisPosition(pnm::units::Angle motor_position) const
{
    const auto displacement{ finite_result(finite_input(motor_position) - m_config.motor_reference) };
    return finite_result(m_config.axis_reference + toAxisDisplacement(displacement));
}

pnm::units::Angle LinearAxisConversion::toMotorPosition(pnm::units::Distance axis_position) const
{
    const auto displacement{ finite_result(finite_input(axis_position) - m_config.axis_reference) };
    return finite_result(m_config.motor_reference + toMotorDisplacement(displacement));
}

pnm::units::Distance LinearAxisConversion::toAxisDisplacement(pnm::units::Angle motor_displacement) const
{
    return convert<pnm::units::Distance>(motor_displacement, m_axisPerMotor);
}

pnm::units::Angle LinearAxisConversion::toMotorDisplacement(pnm::units::Distance axis_displacement) const
{
    return convert<pnm::units::Angle>(axis_displacement, m_motorPerAxis);
}

pnm::units::Velocity LinearAxisConversion::toAxisVelocity(pnm::units::AngularVelocity motor_velocity) const
{
    return convert<pnm::units::Velocity>(motor_velocity, m_axisPerMotor);
}

pnm::units::AngularVelocity LinearAxisConversion::toMotorVelocity(pnm::units::Velocity axis_velocity) const
{
    return convert<pnm::units::AngularVelocity>(axis_velocity, m_motorPerAxis);
}

pnm::units::Velocity LinearAxisConversion::toAxisSpeed(pnm::units::AngularVelocity motor_speed) const
{
    return convert<pnm::units::Velocity>(speed_input(motor_speed), std::abs(m_axisPerMotor));
}

pnm::units::AngularVelocity LinearAxisConversion::toMotorSpeed(pnm::units::Velocity axis_speed) const
{
    return convert<pnm::units::AngularVelocity>(speed_input(axis_speed), std::abs(m_motorPerAxis));
}

pnm::units::Acceleration LinearAxisConversion::toAxisAcceleration(pnm::units::AngularAcceleration motor) const
{
    return convert<pnm::units::Acceleration>(speed_input(motor), std::abs(m_axisPerMotor));
}

pnm::units::AngularAcceleration LinearAxisConversion::toMotorAcceleration(pnm::units::Acceleration axis) const
{
    return convert<pnm::units::AngularAcceleration>(speed_input(axis), std::abs(m_motorPerAxis));
}

pnm::units::Jerk LinearAxisConversion::toAxisJerk(pnm::units::AngularJerk motor) const
{
    return convert<pnm::units::Jerk>(speed_input(motor), std::abs(m_axisPerMotor));
}

pnm::units::AngularJerk LinearAxisConversion::toMotorJerk(pnm::units::Jerk axis) const
{
    return convert<pnm::units::AngularJerk>(speed_input(axis), std::abs(m_motorPerAxis));
}
