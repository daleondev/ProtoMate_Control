#include "AxisConversion.hpp"

#include <cmath>
#include <stdexcept>

namespace
{
    using namespace pnm::units;
    using namespace pnm::units::literals;

    template<IsQuantity Quantity>
    Quantity finiteInput(Quantity value)
    {
        if (!value.isFinite()) {
            throw std::invalid_argument("axis conversion requires finite coordinates and rates");
        }
        return value;
    }

    template<IsQuantity Quantity>
    Quantity finiteResult(Quantity value)
    {
        if (!value.isFinite()) {
            throw std::overflow_error("axis conversion overflow");
        }
        return value;
    }

    template<IsQuantity Quantity>
    Quantity speedInput(Quantity value)
    {
        if (finiteInput(value).get() < 0.0) {
            throw std::invalid_argument("axis speed must be a nonnegative magnitude");
        }
        return value;
    }

    void positiveRatio(double value)
    {
        if (!std::isfinite(value) || value <= 0.0) {
            throw std::invalid_argument("axis transmission ratio must be positive and finite");
        }
    }

    double directionSign(AxisDirection direction)
    {
        switch (direction) {
            case AxisDirection::SameAsMotor:
                return 1.0;
            case AxisDirection::OppositeToMotor:
                return -1.0;
        }
        throw std::invalid_argument("invalid axis direction");
    }

    // Conversion factors use quantity base units (rad, m, rad/s and m/s).
    template<IsQuantity Result, IsQuantity Input>
    Result convert(Input value, double factor)
    {
        return finiteResult(Result::create(finiteInput(value).get() * factor));
    }
}

RotaryAxisConversion::RotaryAxisConversion(Config config)
  : m_config{ config }
{
    positiveRatio(config.motor_revolutions_per_axis_revolution);
    finiteInput(config.motor_reference);
    finiteInput(config.axis_reference);
    m_motorPerAxis = directionSign(config.direction) * config.motor_revolutions_per_axis_revolution;
    m_axisPerMotor = 1.0 / m_motorPerAxis;
    positiveRatio(std::abs(m_axisPerMotor));
}

pnm::units::Angle RotaryAxisConversion::toAxisPosition(pnm::units::Angle motor_position) const
{
    const auto displacement{ finiteResult(finiteInput(motor_position) - m_config.motor_reference) };
    return finiteResult(m_config.axis_reference + toAxisDisplacement(displacement));
}

pnm::units::Angle RotaryAxisConversion::toMotorPosition(pnm::units::Angle axis_position) const
{
    const auto displacement{ finiteResult(finiteInput(axis_position) - m_config.axis_reference) };
    return finiteResult(m_config.motor_reference + toMotorDisplacement(displacement));
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
    return convert<pnm::units::AngularVelocity>(speedInput(motor_speed), std::abs(m_axisPerMotor));
}

pnm::units::AngularVelocity RotaryAxisConversion::toMotorSpeed(pnm::units::AngularVelocity axis_speed) const
{
    return convert<pnm::units::AngularVelocity>(speedInput(axis_speed), std::abs(m_motorPerAxis));
}

LinearAxisConversion::LinearAxisConversion(Config config)
  : m_config{ config }
{
    positiveRatio(config.motor_revolutions_per_output_revolution);
    positiveRatio(config.travel_per_output_revolution.get());
    finiteInput(config.motor_reference);
    finiteInput(config.axis_reference);
    m_axisPerMotor = directionSign(config.direction) * config.travel_per_output_revolution.get() /
                     config.motor_revolutions_per_output_revolution / (1_rev).get();
    positiveRatio(std::abs(m_axisPerMotor));
    m_motorPerAxis = 1.0 / m_axisPerMotor;
    positiveRatio(std::abs(m_motorPerAxis));
}

pnm::units::Distance LinearAxisConversion::toAxisPosition(pnm::units::Angle motor_position) const
{
    const auto displacement{ finiteResult(finiteInput(motor_position) - m_config.motor_reference) };
    return finiteResult(m_config.axis_reference + toAxisDisplacement(displacement));
}

pnm::units::Angle LinearAxisConversion::toMotorPosition(pnm::units::Distance axis_position) const
{
    const auto displacement{ finiteResult(finiteInput(axis_position) - m_config.axis_reference) };
    return finiteResult(m_config.motor_reference + toMotorDisplacement(displacement));
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
    return convert<pnm::units::Velocity>(speedInput(motor_speed), std::abs(m_axisPerMotor));
}

pnm::units::AngularVelocity LinearAxisConversion::toMotorSpeed(pnm::units::Velocity axis_speed) const
{
    return convert<pnm::units::AngularVelocity>(speedInput(axis_speed), std::abs(m_motorPerAxis));
}
