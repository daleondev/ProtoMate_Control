#include "Sink.hpp"
#include "ui/cli/Console.hpp"

namespace ui::logging
{
    bool Sink::isOpen() const { return true; }
    pnm::Result<> Sink::open() { return {}; }
    pnm::Result<> Sink::close() { return {}; }
    pnm::Result<> Sink::flush() { return {}; }

    pnm::Result<std::size_t> Sink::write(std::span<const std::byte> data)
    {
        if (data.empty()) { return 0U; }
        // Character views may alias an existing byte buffer without copying.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        const auto text{ std::string_view{ reinterpret_cast<const char*>(data.data()), data.size() } };
        std::string line{ "[UI] " };
        line += text;
        cli::writeLog(line);
        return data.size();
    }
}
