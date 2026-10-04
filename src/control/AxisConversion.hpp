#pragma once

#include "pneumo/units.hpp"

// Direction of the joint coordinate when the motor's shaft angle increases.
enum class AxisDirection
{
    SameAsMotor,
    OppositeToMotor
};

// Pure coordinate conversions: no motor ownership, homing state or pulse rounding.
// Positions remain unwrapped, including angles beyond one revolution.
// Invalid configuration/nonfinite inputs/negative speeds throw invalid_argument;
// an unrepresentable conversion result throws overflow_error.
class RotaryAxisConversion final
{
  public:
    struct Config
    {
        // E.g. 5 means five motor turns produce one joint turn. Must be positive.
        double motor_revolutions_per_axis_revolution{};
        AxisDirection direction{ AxisDirection::SameAsMotor };
        // These two coordinates describe the same physical location, normally
        // the reference switch. motor_reference must match StepperMotor's
        // configured reference-switch coordinate.
        pnm::units::Angle motor_reference{};
        pnm::units::Angle axis_reference{};
    };

    explicit RotaryAxisConversion(Config config);
    const Config& configuration() const noexcept { return m_config; }

    pnm::units::Angle toAxisPosition(pnm::units::Angle motor_position) const;
    pnm::units::Angle toMotorPosition(pnm::units::Angle axis_position) const;
    pnm::units::Angle toAxisDisplacement(pnm::units::Angle motor_displacement) const;
    pnm::units::Angle toMotorDisplacement(pnm::units::Angle axis_displacement) const;

    // Signed velocities, suitable for commanded and encoder feedback.
    pnm::units::AngularVelocity toAxisVelocity(pnm::units::AngularVelocity motor_velocity) const;
    pnm::units::AngularVelocity toMotorVelocity(pnm::units::AngularVelocity axis_velocity) const;
    // Nonnegative magnitudes: direction inversion does not negate a speed.
    // Use toMotorSpeed for StepperMotor's move/reference/setVelocity arguments.
    pnm::units::AngularVelocity toAxisSpeed(pnm::units::AngularVelocity motor_speed) const;
    pnm::units::AngularVelocity toMotorSpeed(pnm::units::AngularVelocity axis_speed) const;

  private:
    Config m_config;
    double m_axisPerMotor{};
    double m_motorPerAxis{};
};

class LinearAxisConversion final
{
  public:
    struct Config
    {
        // Travel per revolution of the output pulley or screw, before reduction.
        // For a belt: tooth count * belt pitch. For a screw: lead, not thread pitch.
        pnm::units::Distance travel_per_output_revolution{};
        double motor_revolutions_per_output_revolution{ 1.0 };
        AxisDirection direction{ AxisDirection::SameAsMotor };
        pnm::units::Angle motor_reference{};
        pnm::units::Distance axis_reference{};
    };

    explicit LinearAxisConversion(Config config);
    const Config& configuration() const noexcept { return m_config; }

    pnm::units::Distance toAxisPosition(pnm::units::Angle motor_position) const;
    pnm::units::Angle toMotorPosition(pnm::units::Distance axis_position) const;
    pnm::units::Distance toAxisDisplacement(pnm::units::Angle motor_displacement) const;
    pnm::units::Angle toMotorDisplacement(pnm::units::Distance axis_displacement) const;

    pnm::units::Velocity toAxisVelocity(pnm::units::AngularVelocity motor_velocity) const;
    pnm::units::AngularVelocity toMotorVelocity(pnm::units::Velocity axis_velocity) const;
    pnm::units::Velocity toAxisSpeed(pnm::units::AngularVelocity motor_speed) const;
    pnm::units::AngularVelocity toMotorSpeed(pnm::units::Velocity axis_speed) const;

  private:
    Config m_config;
    // Base units: metres per motor radian and motor radians per metre.
    double m_axisPerMotor{};
    double m_motorPerAxis{};
};
