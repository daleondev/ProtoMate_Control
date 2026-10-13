#include "core.hpp"
#include "Catalog.hpp"

#include <utility>

namespace hal::core
{
    namespace
    {
        constexpr auto DEVICES = devices_for(util::executing_core());

        template<typename Result, typename... Parameters, typename... Arguments>
        auto create(Result (*factory)(Parameters...), Arguments&&... arguments) -> Result
        {
            return factory ? factory(std::forward<Arguments>(arguments)...) : Result{};
        }
    }

    auto create_user_button() -> std::shared_ptr<device::IButton> { return create(DEVICES.user_button); }
    auto create_error_led() -> std::shared_ptr<device::ILed> { return create(DEVICES.error_led); }
    auto create_rtc() -> std::shared_ptr<IRtc> { return create(DEVICES.rtc); }
    auto create_rng() -> std::shared_ptr<IRng> { return create(DEVICES.rng); }
    auto create_status_led() -> std::shared_ptr<device::ILed> { return create(DEVICES.status_led); }
    auto create_intercore_channel() -> std::shared_ptr<IInterCoreChannel> { return create(DEVICES.intercore_channel); }
    auto create_runtime_timer() -> std::shared_ptr<ITimer> { return create(DEVICES.runtime_timer); }

    auto create_stepper_generator() -> std::shared_ptr<IStepGenerator>
    { return create(DEVICES.stepper_generator); }
    auto create_stepper_step_output(const std::shared_ptr<IStepGenerator>& generator, MotorId id)
      -> std::shared_ptr<IStepOutput>
    { return create(DEVICES.stepper_step_output, generator, id); }
    auto create_stepper_direction_output(MotorId id) -> std::shared_ptr<IDigitalOutput>
    { return create(DEVICES.stepper_direction_output, id); }
    auto create_steppers_enable_output() -> std::shared_ptr<IDigitalOutput>
    { return create(DEVICES.steppers_enable_output); }
    auto create_reference_limit_switch(MotorId id) -> std::shared_ptr<IDigitalInput>
    { return create(DEVICES.reference_limit_switch, id); }
    auto create_stepper_driver_bus() -> std::shared_ptr<IUart>
    { return create(DEVICES.stepper_driver_bus); }
    auto create_stepper_drivers(const std::array<std::size_t, 3>& microsteps,
                                const std::shared_ptr<IDigitalOutput>& enable,
                                std::shared_ptr<IUart> uart)
      -> std::array<std::shared_ptr<device::IStepperDriver>, 3>
    { return create(DEVICES.stepper_drivers, microsteps, enable, std::move(uart)); }
    auto create_motor_feedback(MotorId id, const MotorFeedbackConfig& config)
      -> std::shared_ptr<device::IMotorFeedback>
    { return create(DEVICES.motor_feedback, id, config); }
    auto create_ethercat_spi() -> std::shared_ptr<ISpi> { return create(DEVICES.ethercat_spi); }
    auto create_ethercat_chip_select() -> std::shared_ptr<IDigitalOutput> { return create(DEVICES.ethercat_chip_select); }
    auto create_ethercat_reset() -> std::shared_ptr<IDigitalOutput> { return create(DEVICES.ethercat_reset); }
    auto create_ethercat_interrupt() -> std::shared_ptr<IDigitalInput> { return create(DEVICES.ethercat_interrupt); }
    auto create_ethercat_sync0() -> std::shared_ptr<IDigitalInput> { return create(DEVICES.ethercat_sync0); }
    auto create_display() -> std::shared_ptr<device::IDisplay> { return create(DEVICES.display); }
    auto create_touch() -> std::shared_ptr<device::ITouch> { return create(DEVICES.touch); }
}
