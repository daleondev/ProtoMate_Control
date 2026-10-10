#pragma once

#include "hal/devices/itf/IMotorFeedback.hpp"
#include "hal/drivers/itf/IDigitalInput.hpp"
#include "hal/drivers/itf/IQuadratureEncoder.hpp"

#include <memory>

namespace hal::device
{
    class QuadratureEncoderFeedback final : public IMotorFeedback
    {
      public:
        QuadratureEncoderFeedback(const QuadratureEncoderFeedback&) = delete;
        auto operator=(const QuadratureEncoderFeedback&) -> QuadratureEncoderFeedback& = delete;
        QuadratureEncoderFeedback(QuadratureEncoderFeedback&&) = delete;
        auto operator=(QuadratureEncoderFeedback&&) -> QuadratureEncoderFeedback& = delete;

        QuadratureEncoderFeedback(std::shared_ptr<IQuadratureEncoder> encoder,
                                  std::uint32_t counts_per_revolution,
                                  std::shared_ptr<IDigitalInput> index = {});
        ~QuadratureEncoderFeedback() override;

        Source source() const noexcept override { return Source::ShaftEncoder; }
        pnm::units::Angle resolution() const noexcept override;
        hal::util::Result<> start() noexcept override;
        hal::util::Result<> stop() noexcept override;
        hal::util::Result<> reference(pnm::units::Angle position) override;
        void invalidate() noexcept override {} // Shaft counts survive driver disable/reset.
        void motion(bool /*running*/,
                    bool /*forward*/,
                    std::chrono::nanoseconds /*step_period*/) noexcept override
        {
        }
        void setCallback(Callback callback) override;

      private:
        void subscribe();
        struct State;
        std::shared_ptr<IQuadratureEncoder> m_encoder;
        // Reserve the attached Z input without implicitly zeroing on its edges.
        std::shared_ptr<IDigitalInput> m_index;
        std::shared_ptr<State> m_state;
    };
}
