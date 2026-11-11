#pragma once
#include <cstddef>
#include <span>

namespace ui::opcua
{
    // Called once, before network/server workers start. Never tests live buffers.
    bool initialize_storage() noexcept;
    std::span<std::byte> server_storage() noexcept;
}
