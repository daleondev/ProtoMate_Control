#pragma once

#include "hal/util/Result.hpp"
#include "pneumo/units.hpp"

#include <chrono>
#include <functional>

namespace hal::device
{
    // Motor-shaft units. This interface does not perform gearing/axis conversion.
    class IMotorFeedback
    {
      public:
        enum class Source
        {
            ShaftEncoder,
            DriverIndex
        };
        struct Sample
        {
            hal::util::Result<pnm::units::Angle> position;
            pnm::units::AngularVelocity velocity{};
            std::chrono::nanoseconds timestamp{};
            // An independent shaft counter lost its coordinate. Coarse INDEX
            // unavailability alone does not invalidate a STEP-counted reference.
            bool reference_lost{};
        };
        using Callback = std::move_only_function<void(const Sample&) noexcept>;

        virtual ~IMotorFeedback() = default;
        IMotorFeedback(const IMotorFeedback&) = delete;
        IMotorFeedback& operator=(const IMotorFeedback&) = delete;

        virtual Source source() const noexcept = 0;
        virtual pnm::units::Angle resolution() const noexcept = 0;
        // Start/stop observation, independently of STEP generation. Creation is
        // stopped. Stopping STEP must not stop an independent shaft encoder.
        virtual hal::util::Result<> start() noexcept = 0;
        virtual hal::util::Result<> stop() noexcept = 0;
        // Rebase at a stopped motor's reference switch; retain hardware counts.
        // Success accepts the coordinate, but does not make an unobserved INDEX
        // valid. Quadrature observation remains running during rebasing.
        virtual hal::util::Result<> reference(pnm::units::Angle position) = 0;
        // Command/driver phase was lost (disable, reset or uncertain STEP count).
        // Independent shaft measurement remains usable; INDEX loses its phase.
        virtual void invalidate() noexcept = 0;
        // ISR-safe motion hint. Period is one STEP pulse, not one INDEX cycle.
        // A shaft encoder measures direction itself and ignores this hint.
        virtual void motion(bool running, bool forward, std::chrono::nanoseconds step_period) noexcept = 0;
        // One subscriber. Registration publishes the latest sample immediately.
        // Callbacks may run in an ISR: no blocking, allocation, or calls into this
        // provider. Thread-context clear/destruction waits for in-flight calls.
        virtual void setCallback(Callback callback) = 0;
        void clearCallback() { setCallback({}); }

      protected:
        IMotorFeedback() = default;
    };
}
