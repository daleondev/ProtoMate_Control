#include "hal/drivers/factory/spi.hpp"
#include "hal/drivers/common.hpp"
#include "spi.h"
#include <limits>

namespace hal::spi
{
    namespace
    {
        class Spi final : public ISpi
        {
          public:
            Spi() { MX_SPI5_Init(); }
            ~Spi() override { static_cast<void>(HAL_SPI_DeInit(&hspi5)); }

            std::uint32_t clockFrequencyHz() const noexcept override
            {
                return HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_SPI5) / 128U;
            }

            util::Result<> exchange(std::span<const std::uint8_t> tx,
                                    std::span<std::uint8_t> rx,
                                    std::chrono::milliseconds timeout) override
            {
                if (__get_IPSR() != 0 || tx.empty() || tx.size() != rx.size() ||
                    tx.size() > std::numeric_limits<std::uint16_t>::max() ||
                    timeout.count() <= 0 || timeout.count() > 1000)
                    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

                const auto status = HAL_SPI_TransmitReceive(&hspi5, tx.data(), rx.data(),
                    static_cast<std::uint16_t>(tx.size()), static_cast<std::uint32_t>(timeout.count()));
                if (status != HAL_OK) {
                    // Discard a partial FIFO transaction. The device wrapper
                    // releases CS on every return, including this error path.
                    __HAL_RCC_SPI5_FORCE_RESET();
                    __HAL_RCC_SPI5_RELEASE_RESET();
                    MX_SPI5_Init();
                }
                return make_result(status);
            }
        };
        std::weak_ptr<ISpi> owner;
    }

    std::shared_ptr<ISpi> createEthercatBus()
    {
        if (!owner.expired()) return {};
        auto bus = std::make_shared<Spi>();
        owner = bus;
        return bus;
    }
}
