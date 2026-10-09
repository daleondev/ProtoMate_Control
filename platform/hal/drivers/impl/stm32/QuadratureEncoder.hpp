#pragma once

#include "hal/drivers/util/EncoderCounter.hpp"
#include "hal/drivers/util/EncoderSampler.hpp"
#include "hal/drivers/util/TimerLease.hpp"
#include "hal/drivers/itf/IDigitalInput.hpp"
#include "hal/hal.hpp"

#include <memory>

namespace hal
{
    class QuadratureEncoder final : public IQuadratureEncoder
    {
      public:
        QuadratureEncoder(util::TimerLease lease,
                          std::shared_ptr<IDigitalInput> a,
                          std::shared_ptr<IDigitalInput> b);
        ~QuadratureEncoder() override;
        [[nodiscard]] auto start() noexcept -> util::Result<> override;
        [[nodiscard]] auto stop() noexcept -> util::Result<> override;
        [[nodiscard]] auto isRunning() const noexcept -> bool override;
        [[nodiscard]] auto position() const noexcept -> util::Result<Count> override;
        [[nodiscard]] auto setPosition(Count count) noexcept -> util::Result<> override;
        auto setSampleCallback(SampleCallback callback) -> void override;
        static auto dispatchInterrupt() noexcept -> void;
        static auto dispatchTimebase() noexcept -> void;

      private:
        auto sample() const noexcept -> void;
        util::TimerLease m_lease;
        std::shared_ptr<IDigitalInput> m_a;
        std::shared_ptr<IDigitalInput> m_b;
        mutable util::EncoderCounter m_counter;
        util::EncoderSampler m_sampler;
        bool m_running{};
        inline static QuadratureEncoder* s_instance{};
    };
}
