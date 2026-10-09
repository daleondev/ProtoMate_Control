#pragma once

#include "hal/drivers/itf/IUart.hpp"
#include "hal/hal.hpp"

namespace hal
{
    class Uart final : public IUart
    {
      public:
        struct HardwareConfiguration
        {
            UART_HandleTypeDef& handle;
            Configuration configuration;
            void (*initialize)();
        };

        explicit Uart(HardwareConfiguration configuration);
        ~Uart() override;

        Uart(const Uart&) = delete;
        Uart& operator=(const Uart&) = delete;
        Uart(Uart&&) = delete;
        Uart& operator=(Uart&&) = delete;

        [[nodiscard]] auto exchange(std::span<const std::uint8_t> request,
                                    std::span<std::uint8_t> reply,
                                    std::chrono::milliseconds timeout) -> util::Result<> override;

      private:
        UART_HandleTypeDef& m_handle;
        Configuration m_configuration;
    };
}
