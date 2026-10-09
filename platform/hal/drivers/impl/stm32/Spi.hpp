#pragma once

#include "hal/drivers/itf/ISpi.hpp"
#include "hal/hal.hpp"

namespace hal
{
    class Spi final : public ISpi
    {
      public:
        struct Configuration
        {
            SPI_HandleTypeDef& handle;
            std::uint32_t (*kernel_clock_hz)();
            void (*initialize)();
            void (*recover)();
        };

        explicit Spi(Configuration configuration);
        ~Spi() override;

        Spi(const Spi&) = delete;
        Spi& operator=(const Spi&) = delete;
        Spi(Spi&&) = delete;
        Spi& operator=(Spi&&) = delete;

        [[nodiscard]] auto exchange(std::span<const std::uint8_t> tx,
                                    std::span<std::uint8_t> rx,
                                    std::chrono::milliseconds timeout) -> util::Result<> override;
        [[nodiscard]] auto clockFrequencyHz() const noexcept -> std::uint32_t override;

      private:
        Configuration m_configuration;
    };
}
