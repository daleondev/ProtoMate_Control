#include "Tmc2209Bus.hpp"
#include "Tmc2209.hpp"
#include <algorithm>
#include <stdexcept>
#include "hal/devices/util/DriverMutex.hpp"

namespace hal::device
{
    // Keep synchronization out of headers: host HAL uses native libstdc++ while
    // application/test code includes the project's ThreadX mutex ABI.
    struct Tmc2209Bus::Lock { util::DriverMutex mutex; };
    Tmc2209Bus::~Tmc2209Bus() = default;
    Tmc2209Bus::Guard::Guard(Tmc2209Bus& bus) : m_bus{ bus } { m_bus.m_lock->mutex.lock(); }
    Tmc2209Bus::Guard::~Guard() { m_bus.m_lock->mutex.unlock(); }
    Tmc2209Bus::Tmc2209Bus(std::shared_ptr<IUart> uart, std::vector<std::uint8_t> addresses)
      : m_uart{ std::move(uart) }, m_addresses{ std::move(addresses) }, m_lock{ std::make_unique<Lock>() }
    {
        if (!m_uart || m_addresses.empty()) throw std::invalid_argument("TMC UART bus unavailable");
        std::ranges::sort(m_addresses);
        if (m_addresses.back() > 3 || std::ranges::adjacent_find(m_addresses) != m_addresses.end())
            throw std::invalid_argument("invalid TMC UART addresses");
    }
    hal::util::Result<> Tmc2209Bus::prepareLocked() { return Tmc2209::prepareBus(*m_uart, m_addresses); }
}
