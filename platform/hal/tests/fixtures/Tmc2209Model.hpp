#pragma once
#include "hal/drivers/itf/IUart.hpp"
#include <array>
#include <atomic>
#include <memory>

namespace hal::device
{
    // Linux model only. Fault injection is deliberately unavailable on STM32.
    class Tmc2209Model final : public IUart
    {
      public:
        Tmc2209Model();
        hal::util::Result<> exchange(std::span<const std::uint8_t> request,
                                     std::span<std::uint8_t> reply,
                                     std::chrono::milliseconds timeout) override;
        void setRegister(unsigned address, unsigned reg, std::uint32_t value);
        std::uint32_t getRegister(unsigned address, unsigned reg) const;
        void setConnected(unsigned address, bool connected);
        void reset(unsigned address);

      private:
        static constexpr auto TMC_REGISTER_COUNT{ 128 };

        std::array<std::array<std::atomic<std::uint32_t>, TMC_REGISTER_COUNT>, 2> m_registers{};
        std::array<std::atomic_bool, 2> m_connected{ true, true };
    };
}
