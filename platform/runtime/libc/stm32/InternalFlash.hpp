#pragma once

#include "runtime/libc/storage/FlashJournal.hpp"

namespace runtime::storage
{
    class InternalFlash final : public FlashDevice
    {
      public:
        bool read(std::size_t offset, std::span<std::byte> output) noexcept override;
        bool program(std::size_t offset, std::span<const std::byte, 32> word) noexcept override;
        bool erase(std::size_t block) noexcept override;
        std::uint32_t error() const noexcept { return m_error; }
      private:
        std::uint32_t m_error{};
    };
}
