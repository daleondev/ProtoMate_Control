#pragma once

#include "hal/drivers/itf/IQuadratureEncoder.hpp"

#include <utility>

namespace hal::detail
{
    // Access is serialized by the encoder backend. Position queries and wrap
    // service must not change the velocity sampling window.
    class EncoderSampler final
    {
      public:
        void setCallback(IQuadratureEncoder::SampleCallback callback) { m_callback = std::move(callback); }

        bool due(std::chrono::nanoseconds now) const noexcept
        {
            return m_callback && now - m_previous >= IQuadratureEncoder::sample_period;
        }

        void publish(const IQuadratureEncoder::Sample& sample) noexcept
        {
            m_previous = sample.timestamp;
            if (m_callback)
                m_callback(sample);
        }

      private:
        IQuadratureEncoder::SampleCallback m_callback;
        std::chrono::nanoseconds m_previous{};
    };
}
