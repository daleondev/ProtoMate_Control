#pragma once

#include "hal/drivers/factory/timer.hpp"

#if defined(HAL_STEP_THREADX)
#include "hal/linux/ThreadMutex.hpp"
#else
#include "hal/linux/Mutex.hpp"
#endif

#include "hal/drivers/detail/EncoderCounter.hpp"
#include "hal/drivers/detail/EncoderSampler.hpp"
#include "hal/drivers/detail/TimerLease.hpp"
#include "hal/drivers/itf/IDigitalInput.hpp"

#include <memory>
#include <mutex>

namespace hal
{
    class QuadratureEncoder final : public IQuadratureEncoder
    {
      public:
        QuadratureEncoder(detail::TimerLease lease,
                          std::shared_ptr<IDigitalInput> a,
                          std::shared_ptr<IDigitalInput> b);
        ~QuadratureEncoder() override;
        [[nodiscard]] auto start() noexcept -> util::Result<> override;
        [[nodiscard]] auto stop() noexcept -> util::Result<> override;
        [[nodiscard]] auto isRunning() const noexcept -> bool override;
        [[nodiscard]] auto position() const noexcept -> util::Result<Count> override;
        [[nodiscard]] auto setPosition(Count count) noexcept -> util::Result<> override;
        auto setSampleCallback(SampleCallback callback) -> void override;
        // Inject signed x4 counts. Movement while stopped is ignored. Samples
        // emulate timely hardware ISR service, including repeated 16-bit wraps.
        [[nodiscard]] auto advanceSimulatedCounts(std::int32_t delta) noexcept -> util::Result<>;

        // Backend service entry, also useful to drive the logical model.
        auto service() noexcept -> void;

      private:
        detail::TimerLease m_lease;
        std::shared_ptr<IDigitalInput> m_a;
        std::shared_ptr<IDigitalInput> m_b;
#if defined(HAL_STEP_THREADX)
        mutable linux::ThreadMutex m_mutex;
#else
        mutable linux::Mutex m_mutex;
#endif
        detail::EncoderCounter m_counter;
        detail::EncoderSampler m_sampler;
        std::uint16_t m_raw{};
        bool m_running{};
        struct Service;
        std::unique_ptr<Service> m_service;
    };

    namespace encoder
    {
        // Return the currently owned encoder for external motion injection.
        // This does not create or replace the exclusive board resource.
        auto simulatedEncoder(timer::Peripheral peripheral) -> std::shared_ptr<QuadratureEncoder>;
    }
}
