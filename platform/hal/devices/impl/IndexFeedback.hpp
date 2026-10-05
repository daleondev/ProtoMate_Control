#pragma once

#include "hal/drivers/itf/IDigitalInput.hpp"
#include "hal/utilities/Result.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

namespace hal::device
{
    // Coarse feedback from a STEP/DIR driver's electrical INDEX, not its shaft.
    // One count is four full steps, independent of microstep configuration.
    class IndexFeedback final
    {
      public:
        struct Sample
        {
            util::Result<std::int64_t> cycles;
            double cycles_per_second{};
            std::chrono::nanoseconds timestamp{};
        };
        using Callback = std::move_only_function<void(const Sample&) noexcept>;
        using Clock = std::chrono::steady_clock;
        using Now = Clock::time_point (*)() noexcept;

        // Input must use both-edge EXTI. The clock parameter permits deterministic tests.
        explicit IndexFeedback(std::shared_ptr<IDigitalInput> input, Now now = &Clock::now);
        ~IndexFeedback();
        IndexFeedback(const IndexFeedback&) = delete;
        IndexFeedback& operator=(const IndexFeedback&) = delete;

        // Called by STEP progress callbacks. No STEP pulses are counted here.
        // Direction changes/resumes discard the old velocity interval. A stop
        // publishes zero velocity and still accepts the final interpolated edge.
        void motion(bool running, bool forward, std::chrono::nanoseconds cycle_period) noexcept;
        // Disable/reconfigure/reset loses the driver's electrical phase.
        void invalidate() noexcept;
        // Rebase while stopped at the reference switch. Sub-cycle position is unresolved;
        // feedback stays unavailable until an INDEX transition has been seen.
        void reference() noexcept;
        // One subscriber. Callbacks are serialized and may run in EXTI/progress
        // interrupt context: no blocking, allocation, or calls into this object.
        // Clearing waits for an in-flight callback. Thread-context configuration only.
        void setCallback(Callback callback);
        void clearCallback() { setCallback({}); }

      private:
        struct State;
        std::shared_ptr<State> m_state;
        std::shared_ptr<IDigitalInput> m_input;
    };
}
