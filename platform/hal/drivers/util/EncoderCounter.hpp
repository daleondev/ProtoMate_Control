#pragma once

#include "hal/drivers/itf/IQuadratureEncoder.hpp"

#include <cstdint>
#include <limits>
#include <system_error>

namespace hal::util
{
    // Extends a 16-bit hardware counter. Consecutive samples must be separated
    // by strictly less than 32768 net counts. STM32 uses internal compare/update
    // interrupts to service this even if application code never reads position.
    class EncoderCounter final
    {
      public:
        auto sample(std::uint16_t raw) noexcept -> void
        {
            std::int32_t delta{ static_cast<std::int32_t>(raw) - m_previous };
            m_previous = raw;
            if (delta == 32768 || delta == -32768) {
                m_error = std::errc::result_out_of_range;
                return;
            }
            if (delta > 32768) {
                delta -= 65536;
            }
            if (delta < -32768) {
                delta += 65536;
            }
            if (m_error != std::errc{}) {
                return;
            }
            if ((delta > 0 && m_count > std::numeric_limits<std::int64_t>::max() - delta) ||
                (delta < 0 && m_count < std::numeric_limits<std::int64_t>::min() - delta)) {
                m_error = std::errc::value_too_large;
                return;
            }
            m_count += delta;
        }
        auto reset(std::int64_t count, std::uint16_t raw) noexcept -> void
        {
            m_count = count;
            m_previous = raw;
            m_error = {};
        }
        [[nodiscard]] auto position() const noexcept -> util::Result<std::int64_t>
        {
            if (m_error != std::errc{}) {
                return std::unexpected(std::make_error_code(m_error));
            }
            return m_count;
        }

      private:
        std::int64_t m_count{};
        std::uint16_t m_previous{};
        std::errc m_error{};
    };
}
