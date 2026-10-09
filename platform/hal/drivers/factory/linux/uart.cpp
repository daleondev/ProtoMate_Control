#include "hal/drivers/factory/uart.hpp"
#include "hal/drivers/detail/UartTransfer.hpp"
#include "hal/drivers/impl/linux/Uart.hpp"
#include "hal/linux/Mutex.hpp"
#include <array>
#include <mutex>
#include <utility>

namespace hal::uart
{
    auto create(Configuration configuration) -> std::shared_ptr<IUart>
    {
        const auto index{ std::to_underlying(configuration.peripheral) };
        if (configuration.peripheral == Peripheral::Usart3 ||
            !detail::validUartConfiguration(configuration.transport) || index >= 9U)
            return {};
        static linux::Mutex mutex;
        static std::array<std::weak_ptr<IUart>, 9> owners;
        const std::scoped_lock lock{ mutex };
        auto& owner{ owners[index] };
        if (!owner.expired())
            return {};
        auto bus{ std::make_shared<Uart>(configuration.transport) };
        owner = bus;
        return bus;
    }
}
