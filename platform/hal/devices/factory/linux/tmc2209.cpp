#include "hal/devices/factory/tmc2209.hpp"
#include "simulation/Client.hpp"

#include "hal/drivers/factory/uart.hpp"
#include "hal/drivers/impl/linux/Uart.hpp"
#include <tx_api.h>

namespace hal::device::tmc2209
{
    void configure_fixture_uart(const std::shared_ptr<IUart>&) __attribute__((weak));
    auto create_transport(uart::Configuration configuration) -> std::shared_ptr<IUart>
    {
        auto transport{ uart::create(configuration) };
        if (transport && simulation::client::connected()) {
            std::static_pointer_cast<Uart>(transport)->setExchangeHandler(
              [configuration](auto tx, auto rx, auto timeout) -> hal::util::Result<> {
                // USART2 is fixed at 115200 baud, 8N1. Echo occupies the same wire
                // interval as TX; device turnaround is bounded conservatively.
                const auto microseconds{ (tx.size() + rx.size()) * 10U * 1'000'000U / 115200U + 260U };
                const auto ticks{ static_cast<ULONG>((microseconds + 999) / 1000 +
                                                     configuration.transport.settle_time.count()) };
                if (ticks >= static_cast<ULONG>(timeout.count()))
                    return std::unexpected(std::make_error_code(std::errc::timed_out));
                if (tx_thread_sleep(ticks) != TX_SUCCESS)
                    return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
                simulation::client::Transaction guard{};
                simulation::Packet q{ .operation = simulation::Operation::Uart,
                                      .size = static_cast<std::uint32_t>(tx.size()),
                                      .arguments = { rx.size() } };
                if (tx.size() > q.data.size())
                    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
                for (unsigned i{}; i < tx.size(); ++i)
                    q.data[i] = std::byte{ tx[i] };
                const auto r{ simulation::client::call(q) };
                if (r.status)
                    return std::unexpected(
                      std::error_code{ static_cast<int>(r.status), std::generic_category() });
                if (r.size != rx.size())
                    return std::unexpected(std::make_error_code(std::errc::protocol_error));
                for (unsigned i{}; i < rx.size(); ++i)
                    rx[i] = std::to_integer<std::uint8_t>(r.data[i]);
                return {};
            });
            return transport;
        }
        if (transport && configure_fixture_uart)
            configure_fixture_uart(transport);
        return transport;
    }
}
