#pragma once

#include "hal/drivers/itf/IUart.hpp"
#include "hal/drivers/itf/IDigitalInput.hpp"
#include "hal/drivers/itf/IDigitalOutput.hpp"
#include "hal/hal.hpp"
#include <memory>

namespace hal
{
    class Uart final : public IUart
    {
      public:
        struct HardwareConfiguration
        {
            UART_HandleTypeDef& handle; // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members) --
                                        // Required, non-owning dependency; its owner outlives this object.
            Configuration configuration;
            std::shared_ptr<IDigitalOutput> tx;
            std::shared_ptr<IDigitalInput> rx;
        };

        explicit Uart(HardwareConfiguration configuration);
        ~Uart() override = default;

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
        std::shared_ptr<IDigitalOutput> m_tx;
        std::shared_ptr<IDigitalInput> m_rx;
    };
}
