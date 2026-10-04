#include "MotionController.hpp"

#include <stdexcept>
#include <type_traits>

namespace control
{
    namespace
    {
        template<typename T, typename... Alternatives>
        const T& typed(const std::variant<Alternatives...>& value)
        {
            if (const auto* result{ std::get_if<T>(&value) })
                return *result;
            throw std::invalid_argument("axis units do not match the configured rotary/linear axis");
        }

        template<typename Converter>
        using Speed = decltype(std::declval<Converter>().toAxisSpeed(0_rpm));

        template<typename Converter>
        using AxisRequest = std::conditional_t<std::is_same_v<Converter, RotaryAxisConversion>,
                                               MotionController::Move,
                                               MotionController::LinearMove>;

        template<typename Converter>
        using Defaults = std::conditional_t<std::is_same_v<Converter, RotaryAxisConversion>,
                                            StepperMotor::MotionDefaults,
                                            MotionController::LinearDefaults>;
    }

    MotionController::AxisConfig MotionController::motorConfiguration(MotorId motor) const
    {
        return m_configuration.at(static_cast<std::size_t>(motor));
    }

    void MotionController::configureAxis(MotorId motor, Conversion conversion)
    {
        std::scoped_lock lock{ m_mutex };
        const auto config{ motorConfiguration(motor) };
        if ((motor == MotorId::Motor3) != std::holds_alternative<LinearAxisConversion>(conversion))
            throw std::invalid_argument(
              "shoulder/elbow require rotary conversion; Z requires linear conversion");
        std::visit([&](const auto& value) {
            if (value.configuration().motor_reference != config.reference_position)
                throw std::invalid_argument(
                  "axis motor reference must match the motor's reference coordinate");
        }, conversion);
        if (m_enable->read() != hal::gpio::Level::High)
            throw std::runtime_error("disable drivers before changing axis conversion");
        m_conversions[static_cast<std::size_t>(motor)] = std::move(conversion);
    }

    const MotionController::Conversion& MotionController::conversionLocked(MotorId motor) const
    {
        const auto& value{ m_conversions.at(static_cast<std::size_t>(motor)) };
        if (!value)
            throw std::runtime_error("axis mechanics are not configured in the application");
        return *value;
    }

    MotionController::Conversion MotionController::axisConversion(MotorId motor) const
    {
        std::scoped_lock lock{ m_mutex };
        return conversionLocked(motor);
    }

    MotionController::Motion MotionController::moveAxis(MotorId motor, const AxisMove& request)
    {
        std::scoped_lock lock{ m_mutex };
        return std::visit([&]<typename Converter>(const Converter& conversion) {
            const auto& value{ typed<AxisRequest<Converter>>(request) };
            const Move converted{
                .position = value.absolute ? conversion.toMotorPosition(value.position)
                                           : conversion.toMotorDisplacement(value.position),
                .velocity = conversion.toMotorSpeed(value.velocity),
                .acceleration = conversion.toMotorAcceleration(value.acceleration),
                .deceleration = conversion.toMotorAcceleration(value.deceleration),
                .jerk = conversion.toMotorJerk(value.jerk),
                .buffer = value.buffer,
                .timeout = value.timeout,
                .absolute = value.absolute,
            };
            return moveLocked(motor, converted);
        }, conversionLocked(motor));
    }

    MotionController::Motion MotionController::referenceAxis(MotorId motor,
                                                             std::optional<AxisSpeed> seek,
                                                             std::optional<AxisSpeed> latch,
                                                             pnm::units::Time timeout)
    {
        std::scoped_lock lock{ m_mutex };
        return std::visit([&]<typename Converter>(const Converter& conversion) {
            const auto seek_motor{ seek ? conversion.toMotorSpeed(typed<Speed<Converter>>(*seek)) : 5_rpm };
            const auto latch_motor{ latch ? conversion.toMotorSpeed(typed<Speed<Converter>>(*latch))
                                          : 0.5_rpm };
            if (latch_motor >= seek_motor)
                throw std::invalid_argument("latch speed must be slower than seek speed");
            return referenceLocked(motor, seek_motor, latch_motor, timeout);
        }, conversionLocked(motor));
    }

    hal::step::PulseCount MotionController::setAxisVelocity(MotorId motor, const AxisSpeed& velocity)
    {
        std::scoped_lock lock{ m_mutex };
        return std::visit([&]<typename Converter>(const Converter& conversion) {
            return setVelocityLocked(motor, conversion.toMotorSpeed(typed<Speed<Converter>>(velocity)));
        }, conversionLocked(motor));
    }

    MotionController::AxisDefaults MotionController::axisDefaults(MotorId motor) const
    {
        std::scoped_lock lock{ m_mutex };
        const auto defaults{ axis(motor).motionDefaults() };
        return std::visit([&]<typename Converter>(const Converter& conversion) -> AxisDefaults {
            return Defaults<Converter>{ conversion.toAxisAcceleration(defaults.acceleration),
                                        conversion.toAxisAcceleration(defaults.deceleration),
                                        conversion.toAxisJerk(defaults.jerk) };
        }, conversionLocked(motor));
    }

    void MotionController::setAxisDefaults(MotorId motor, const AxisDefaults& defaults)
    {
        std::scoped_lock lock{ m_mutex };
        const auto converted{ std::visit([&]<typename Converter>(const Converter& conversion) {
            const auto& value{ typed<Defaults<Converter>>(defaults) };
            return StepperMotor::MotionDefaults{ conversion.toMotorAcceleration(value.acceleration),
                                                 conversion.toMotorAcceleration(value.deceleration),
                                                 conversion.toMotorJerk(value.jerk) };
        }, conversionLocked(motor)) };
        if (const auto result{ axis(motor).setMotionDefaults(converted) }; !result)
            throw std::runtime_error("cannot set axis motion defaults: " + result.error().message());
    }
}
