#include "StepperMotor.hpp"
#include "pneumo/logging.hpp"
#include <stdexcept>
#include <utility>

namespace io::motion
{
    StepperMotor::StepperMotor(hal::core::MotorId id,
                               pnm::units::Angle reference_switch_position,
                               pnm::units::Angle full_step_angle,
                               size_t microsteps,
                               const std::shared_ptr<hal::IStepGenerator>& step_generator,
                               std::shared_ptr<hal::device::IStepperDriver> driver,
                               std::shared_ptr<hal::device::IMotorFeedback> feedback,
                               hal::device::IStepperDriver::FaultCallback shutdown,
                               std::shared_ptr<pnm::log::ISink> log_sink,
                               std::shared_ptr<hal::IDigitalInput> reference_input,
                               Direction reference_direction)
      : m_id{ id }
      , m_referenceSwitchPosition{ reference_switch_position }
      , m_referenceDirection{ reference_direction }
      , m_fullStepAngle{ full_step_angle }
      , m_microsteps{ microsteps }
      , m_stepOutput{ hal::core::create_stepper_step_output(step_generator, id) }
      , m_dirOutput{ hal::core::create_stepper_direction_output(id) }
      , m_referenceSwitchInput{ reference_input ? std::move(reference_input)
                                                : hal::core::create_reference_limit_switch(id) }
      , m_feedback{ feedback ? std::move(feedback)
                             : hal::core::create_motor_feedback(
                                 id,
                                 { .full_step_angle = full_step_angle, .microsteps = microsteps }) }
      , m_driver{ std::move(driver) }
      , m_logSink{ std::move(log_sink) }
    {
        // Pneumo assertions implement failure reporting through a C macro.
        // NOLINTBEGIN(cppcoreguidelines-avoid-do-while,cppcoreguidelines-pro-type-vararg,modernize-use-std-print,readability-simplify-boolean-expr)
        PNM_ASSERT(m_referenceSwitchInput, "Motor %d has no reference switch", static_cast<int>(id) + 1);
        // NOLINTEND(cppcoreguidelines-avoid-do-while,cppcoreguidelines-pro-type-vararg,modernize-use-std-print,readability-simplify-boolean-expr)

        if (!reference_switch_position.isFinite() || !full_step_angle.isFinite() ||
            full_step_angle <= 0_deg || microsteps == 0U || reference_direction > Direction::Backward) {
            throw std::invalid_argument("invalid reference position, step angle or microstep count");
        }
        m_stepAngle = m_fullStepAngle / static_cast<double>(m_microsteps);

        if (!m_stepOutput || !m_dirOutput || !m_referenceSwitchInput || !m_feedback || !m_driver) {
            throw std::runtime_error("motor board resource is unavailable");
        }

        m_events->shutdown = std::move(shutdown);
        installCallbacks();
    }

}
