#include "Sink.hpp"
#include "common/icc/catalog/topics.hpp"

namespace io::logging
{
    Sink::Sink(pnm::msg::Bus& bus) : m_topic{ bus.topic<^^icc::Topics::io_logs>() }
    {
        colors(pnm::log::ColorMode::Never);
    }

    bool Sink::isOpen() const
    {
        std::scoped_lock lock{ m_mutex };
        return m_topic.has_value();
    }

    pnm::Result<> Sink::open()
    {
        if (isOpen()) { return {}; }
        return std::unexpected(std::make_error_code(std::errc::operation_canceled));
    }

    pnm::Result<> Sink::close()
    {
        std::scoped_lock lock{ m_mutex };
        m_topic.reset();
        return {};
    }

    pnm::Result<std::size_t> Sink::write(std::span<const std::byte> data)
    {
        std::scoped_lock lock{ m_mutex };
        if (!m_topic) {
            return std::unexpected(std::make_error_code(std::errc::operation_canceled));
        }
        if (data.empty()) { return 0U; }
        // Character views may alias a byte buffer.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        const auto text{ std::string_view{ reinterpret_cast<const char*>(data.data()), data.size() } };
        std::string record;
        if (text.size() <= icc::MAX_LOG_BYTES) {
            record = text;
        }
        else {
            constexpr std::string_view suffix{ "... [truncated]\n" };
            record = text.substr(0, icc::MAX_LOG_BYTES - suffix.size());
            record += suffix;
            m_truncated.fetch_add(1, std::memory_order_relaxed);
        }
        m_topic->publish(record);
        // A truncated record was consumed in full; never retry the remainder.
        return data.size();
    }

    pnm::Result<> Sink::flush() { return {}; } // Best effort, never wait for delivery.

    std::size_t Sink::truncatedRecords() const noexcept
    {
        return m_truncated.load(std::memory_order_relaxed);
    }
}
