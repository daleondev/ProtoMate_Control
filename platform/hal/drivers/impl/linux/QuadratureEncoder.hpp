#pragma once

#include "hal/drivers/detail/EncoderCounter.hpp"
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
        [[nodiscard]] auto start() noexcept -> util::Result<> override;
        [[nodiscard]] auto stop() noexcept -> util::Result<> override;
        [[nodiscard]] auto isRunning() const noexcept -> bool override;
        [[nodiscard]] auto position() const noexcept -> util::Result<Count> override;
        [[nodiscard]] auto setPosition(Count count) noexcept -> util::Result<> override;
        // Inject signed x4 counts. Movement while stopped is ignored. Samples
        // emulate timely hardware ISR service, including repeated 16-bit wraps.
        [[nodiscard]] auto advanceSimulatedCounts(std::int32_t delta) noexcept -> util::Result<>;

      private:
        detail::TimerLease m_lease;
        std::shared_ptr<IDigitalInput> m_a;
        std::shared_ptr<IDigitalInput> m_b;
        mutable std::mutex m_mutex;
        detail::EncoderCounter m_counter;
        std::uint16_t m_raw{};
        bool m_running{};
    };
}
