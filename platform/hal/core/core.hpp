#pragma once
#include "hal/drivers/itf/IGraphics.hpp"
#include "hal/devices/itf/IDisplay.hpp"
#include "hal/devices/itf/ITouch.hpp"

#include "hal/devices/itf/ILed.hpp"
#include "hal/devices/itf/IButton.hpp"
#include "hal/drivers/itf/IRtc.hpp"
#include "hal/drivers/itf/IRng.hpp"
#include "hal/drivers/itf/ITimer.hpp"
#include "hal/util/Core.hpp"
#include "hal/drivers/itf/IInterCoreChannel.hpp"
#include "hal/devices/itf/IMotorFeedback.hpp"
#include "hal/devices/itf/IStepperDriver.hpp"
#include "hal/devices/itf/MotorTypes.hpp"
#include "hal/drivers/itf/IDigitalInput.hpp"
#include "hal/drivers/itf/IDigitalOutput.hpp"
#include "hal/drivers/itf/IStepGenerator.hpp"
#include "hal/drivers/itf/IUart.hpp"

#include "hal/drivers/itf/ISpi.hpp"
#include <memory>
#include <array>

namespace hal::core
{
    using Id = util::Core;

    [[nodiscard]] constexpr auto id() noexcept -> Id { return util::executing_core(); }

    // Select construction recipes from Catalog using util::executing_core().
    // Unassigned roles return nullptr. There is no peer-core selector here.
    [[nodiscard]] auto create_user_button() -> std::shared_ptr<device::IButton>;
    [[nodiscard]] auto create_error_led() -> std::shared_ptr<device::ILed>;
    [[nodiscard]] auto create_rtc() -> std::shared_ptr<IRtc>;
    [[nodiscard]] auto create_rng() -> std::shared_ptr<IRng>;
    [[nodiscard]] auto create_status_led() -> std::shared_ptr<device::ILed>;
    [[nodiscard]] auto create_intercore_channel() -> std::shared_ptr<IInterCoreChannel>;
    [[nodiscard]] auto create_runtime_timer() -> std::shared_ptr<ITimer>;

    using MotorId = motor::Id;
    using MotorFeedbackConfig = motor::FeedbackConfiguration;

    // Available on M4 (and the Host test platform). M7 returns empty results.
    // Construction does not start pulses, initialize drivers or enable motors.
    [[nodiscard]] auto create_stepper_generator() -> std::shared_ptr<IStepGenerator>;
    [[nodiscard]] auto create_stepper_step_output(const std::shared_ptr<IStepGenerator>& generator,
                                                 MotorId id) -> std::shared_ptr<IStepOutput>;
    [[nodiscard]] auto create_stepper_direction_output(MotorId id) -> std::shared_ptr<IDigitalOutput>;
    [[nodiscard]] auto create_steppers_enable_output() -> std::shared_ptr<IDigitalOutput>;
    [[nodiscard]] auto create_reference_limit_switch(MotorId id) -> std::shared_ptr<IDigitalInput>;
    [[nodiscard]] auto create_stepper_driver_bus() -> std::shared_ptr<IUart>;
    [[nodiscard]] auto create_stepper_drivers(const std::array<std::size_t, 3>& microsteps,
                                             const std::shared_ptr<IDigitalOutput>& enable,
                                             std::shared_ptr<IUart> uart = {})
      -> std::array<std::shared_ptr<device::IStepperDriver>, 3>;
    [[nodiscard]] auto create_motor_feedback(MotorId id, const MotorFeedbackConfig& config)
      -> std::shared_ptr<device::IMotorFeedback>;
    [[nodiscard]] auto create_ethercat_reset() -> std::shared_ptr<IDigitalOutput>;
    [[nodiscard]] auto create_ethercat_interrupt() -> std::shared_ptr<IDigitalInput>;
    [[nodiscard]] auto create_ethercat_sync0() -> std::shared_ptr<IDigitalInput>;
    [[nodiscard]] auto create_ethercat_spi() -> std::shared_ptr<ISpi>;
    [[nodiscard]] auto create_ethercat_chip_select() -> std::shared_ptr<IDigitalOutput>;
    [[nodiscard]] auto create_graphics() -> std::shared_ptr<IGraphics>;
    [[nodiscard]] auto create_display() -> std::shared_ptr<device::IDisplay>;
    [[nodiscard]] auto create_touch() -> std::shared_ptr<device::ITouch>;
}
