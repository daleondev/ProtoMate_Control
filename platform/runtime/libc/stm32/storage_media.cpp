#include "runtime/libc/storage_media.hpp"

// Full FileX/syscall support is linked into both runtimes. This board has no
// assigned storage medium yet; opening /flash or /sd reports an absent volume.
// Never emulate a persistent disk or touch an unassigned external bus.
extern "C" {
    runtime::storage::Diagnostics runtime_storage_diagnostics{};
}
namespace runtime::storage
{
    bool initialize() noexcept { return false; }
    FX_MEDIA* media(Volume) noexcept { return nullptr; }
    bool mounted(Volume) noexcept { return false; }
    const Diagnostics& diagnostics() noexcept { return runtime_storage_diagnostics; }
}
