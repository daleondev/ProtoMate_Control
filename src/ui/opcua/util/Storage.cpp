#include "Storage.hpp"
#include <array>
#include <cstdint>
#if defined(CORE_CM7)
#include "stm32h7xx_hal.h"
#include "network/ByteAccess.h"
#endif

namespace ui::opcua
{
    namespace
    {
        alignas(32)
#if defined(CORE_CM7)
        __attribute__((section(".opcua_sdram")))
#endif
        // Host metadata carries 64-bit pointers/size_t and larger allocator
        // headers. Keep the same bounded node/session capacities without
        // pretending its byte footprint equals the 32-bit target's.
        std::array<std::byte, 512U * 1024U * (sizeof(void*) / 4)> arena;
#if defined(CORE_CM7)
        static_assert(sizeof(arena) == 512U * 1024U);
#endif
    }
    std::span<std::byte> server_storage() noexcept { return arena; }
    bool initialize_storage() noexcept
    {
#if defined(CORE_CM7)
        // Verify the entire reservation, then exercise byte routines with
        // independently varied source/destination alignments and cache misses.
        auto* words{ reinterpret_cast<volatile std::uint32_t*>(arena.data()) };
        constexpr auto count{ arena.size() / sizeof(std::uint32_t) };
        for (std::size_t i{ 0 }; i < count; ++i) words[i] = 0xA55A37C9U ^ static_cast<std::uint32_t>(i);
        SCB_CleanInvalidateDCache_by_Addr(arena.data(), static_cast<std::int32_t>(arena.size()));
        for (std::size_t i{ 0 }; i < count; ++i)
            if (words[i] != (0xA55A37C9U ^ static_cast<std::uint32_t>(i))) return false;
        auto* bytes{ reinterpret_cast<unsigned char*>(arena.data()) };
        for (unsigned source{ 0 }; source < 32; ++source) {
            for (unsigned destination{ 0 }; destination < 32; ++destination) {
                for (unsigned i{ 0 }; i < 257; ++i) bytes[source + i] = static_cast<unsigned char>(i * 37U + 11U);
                SCB_CleanInvalidateDCache_by_Addr(arena.data(), 1024);
                network_memcpy(bytes + 512 + destination, bytes + source, 257);
                SCB_CleanInvalidateDCache_by_Addr(arena.data(), 1024);
                for (unsigned i{ 0 }; i < 257; ++i)
                    if (bytes[512 + destination + i] != static_cast<unsigned char>(i * 37U + 11U)) return false;
                if (network_memcmp(bytes + source, bytes + 512 + destination, 257)) return false;
                network_memmove(bytes + 513 + destination, bytes + 512 + destination, 257);
                if (network_memcmp(bytes + source, bytes + 513 + destination, 257)) return false;
            }
        }
        for (unsigned offset{ 0 }; offset < 32; ++offset) {
            network_strcpy(reinterpret_cast<char*>(bytes + offset), "ProtoMate SDRAM access check");
            SCB_CleanInvalidateDCache_by_Addr(arena.data(), 64);
            if (network_strlen(reinterpret_cast<char*>(bytes + offset)) != sizeof("ProtoMate SDRAM access check") - 1 ||
                network_strcmp(reinterpret_cast<char*>(bytes + offset), "ProtoMate SDRAM access check")) return false;
        }
#endif
        return true;
    }
}
