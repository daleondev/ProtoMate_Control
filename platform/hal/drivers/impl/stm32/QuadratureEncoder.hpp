#pragma once

#include "hal/drivers/detail/EncoderCounter.hpp"
#include "hal/drivers/detail/TimerLease.hpp"
#include "hal/drivers/itf/IDigitalInput.hpp"
#include "hal/hal.hpp"

#include <memory>

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
        static auto dispatchInterrupt() noexcept -> void;

      private:
        auto sample() const noexcept -> void;
        detail::TimerLease m_lease;
        std::shared_ptr<IDigitalInput> m_a;
        std::shared_ptr<IDigitalInput> m_b;
        mutable detail::EncoderCounter m_counter;
        bool m_running{};
        inline static QuadratureEncoder* s_instance{};
    };
}
