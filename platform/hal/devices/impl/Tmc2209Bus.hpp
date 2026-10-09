#pragma once

#include "hal/drivers/itf/IUart.hpp"
#include <memory>
#include <vector>

namespace hal::device
{
    class Tmc2209Driver;
    // One shared transaction domain. Bootstrap ALL addresses before reads after
    // a reset; hold the lock over complete verification/configuration sequences.
    class Tmc2209Bus final
    {
      public:
        Tmc2209Bus(std::shared_ptr<IUart> uart, std::vector<std::uint8_t> addresses);
        ~Tmc2209Bus();
      private:
        friend class Tmc2209Driver;
        hal::util::Result<> prepareLocked();
        std::shared_ptr<IUart> m_uart;
        std::vector<std::uint8_t> m_addresses;
        struct Lock;
        std::unique_ptr<Lock> m_lock;
        class Guard
        {
          public:
            explicit Guard(Tmc2209Bus& bus);
            ~Guard();
            Guard(const Guard&) = delete;
            Guard& operator=(const Guard&) = delete;
          private:
            Tmc2209Bus& m_bus;
        };
    };
}
