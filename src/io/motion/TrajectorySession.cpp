#include "TrajectorySession.hpp"

#include <algorithm>
#include <stdexcept>

namespace io::motion
{
    namespace tr = ::motion::trajectory;
    namespace
    {
        constexpr std::uint8_t ALL_AXES{ 0b111 };
    }
    TrajectorySession::TrajectorySession(ITrajectoryExecutor& executor, std::uint64_t boot)
      : m_executor{ &executor }
      , m_boot{ boot }
      , m_banks{ std::make_shared<TrajectoryWindow>(), std::make_shared<TrajectoryWindow>() }
    {
        if (!boot) {
            throw std::invalid_argument("trajectory boot identity must be nonzero");
        }
    }
    tr::Reply TrajectorySession::reply(tr::Error error)
    {
        auto r{ m_executor->snapshot() };
        r.boot = m_boot;
        r.session = m_session;
        r.error = error;
        r.revision = m_revision;
        const auto& w{ *m_banks[m_staging] };
        r.next_index = w.continuation;
        r.stopping_index = w.stopping;
        r.credits =
          m_banks[m_staging].use_count() == 1 ? TrajectoryWindow::CAPACITY - w.continuation - w.stopping : 0;
        return r;
    }
    void TrajectorySession::invalidate() noexcept
    {
        m_session = 0;
        m_configuration = 0;
        m_revision = 0;
        m_chunkCount = 0;
        for (auto& bank : m_banks) {
            if (bank.use_count() == 1) {
                bank->continuation = bank->stopping = 0;
                bank->continuation_final = bank->stopping_final = false;
                bank->continuation_ticks = bank->total_ticks = bank->revision = 0;
            }
        }
    }
    tr::Packet TrajectorySession::request(const tr::Packet& packet)
    {
        const auto decoded{ tr::decode_request(packet) };
        return tr::encode(decoded ? request(*decoded) : reply(decoded.error()));
    }
    tr::Reply TrajectorySession::acquire(const tr::Request& r, const tr::Reply& current)
    {
        if (m_session || !r.configuration) {
            return reply(tr::Error::Busy);
        }
        if (!current.capabilities.coordinated_execution) {
            return reply(tr::Error::Unsupported);
        }
        if (!current.enabled || current.state != tr::State::Ready || !current.counts_exact) {
            return reply(tr::Error::Fault);
        }
        if (current.referenced != ALL_AXES) {
            return reply(tr::Error::Unreferenced);
        }
        if (current.position != r.expected_position) {
            return reply(tr::Error::Stale);
        }
        const auto session{ m_nextSession++ };
        const auto error{ m_executor->acquire(session) };
        if (error != tr::Error::None) {
            return reply(error);
        }
        m_session = session;
        m_generation = r.generation;
        m_configuration = r.configuration;
        m_anchor = current.position;
        m_revision = 0;
        m_chunkCount = 0;
        for (auto& bank : m_banks) {
            if (bank.use_count() != 1) {
                invalidate();
                m_executor->release(session);
                return reply(tr::Error::Busy);
            }
            bank->continuation = bank->stopping = 0;
            bank->continuation_final = bank->stopping_final = false;
            bank->continuation_ticks = bank->total_ticks = bank->revision = 0;
        }
        return reply();
    }
    tr::Reply TrajectorySession::request(const tr::Request& r)
    {
        if (r.version != tr::VERSION || r.operation > tr::Operation::Home) {
            return reply(tr::Error::Protocol);
        }
        if (r.operation == tr::Operation::Inspect) {
            return reply();
        }
        if (r.boot != m_boot) {
            return reply(tr::Error::Stale);
        }
        const auto current{ m_executor->snapshot() };
        if (r.operation == tr::Operation::Abort || r.operation == tr::Operation::Disable) {
            const auto error{ m_executor->control(r.operation) };
            invalidate();
            return reply(error);
        }
        if (r.generation != current.generation) {
            return reply(tr::Error::Stale);
        }
        if (r.operation == tr::Operation::Acquire) {
            return acquire(r, current);
        }

        if (!m_session || r.session != m_session || r.configuration != m_configuration ||
            r.generation != m_generation) {
            return reply(tr::Error::Stale);
        }
        if (r.operation == tr::Operation::Upload) {
            return reply(upload(r));
        }
        if (r.operation == tr::Operation::Commit) {
            return reply(commit(r));
        }
        if (r.operation == tr::Operation::Release) {
            const auto error{ m_executor->release(m_session) };
            if (error == tr::Error::None) {
                invalidate();
            }
            return reply(error);
        }
        return reply(m_executor->control(r.operation));
    }
    std::expected<std::uint64_t, tr::Error> TrajectorySession::validateChunk(
      const tr::Request& r,
      const tr::Capabilities& hardware) const
    {
        const auto& w{ *m_banks[m_staging] };
        const auto first{ std::size_t(w.continuation) + w.stopping };
        std::uint64_t added_ticks{ 0 };
        // Validate the complete chunk before making any of it visible.
        for (unsigned i{ 0 }; i < r.count; ++i) {
            const auto& segment{ r.segments[i] };
            const auto error{ tr::validate(segment, hardware.limits) };
            if (error != tr::Error::None) {
                return std::unexpected(error);
            }
            const tr::Segment* preceding{};
            if (i) {
                preceding = &r.segments[i - 1];
            }
            else if (first) {
                preceding = &w.segments[first - 1];
            }
            if (preceding && !tr::continuous(preceding->at(1), segment.at(0))) {
                return std::unexpected(tr::Error::Discontinuity);
            }
            if (!preceding && m_revision == 0) {
                tr::MotorStates anchor{};
                for (unsigned j{ 0 }; j < anchor.size(); ++j) {
                    anchor[j].position = static_cast<double>(m_anchor[j]);
                }
                if (!tr::continuous(anchor, segment.at(0))) {
                    return std::unexpected(tr::Error::Discontinuity);
                }
            }
            added_ticks += segment.ticks;
        }
        return added_ticks;
    }
    std::optional<tr::Error> TrajectorySession::repeatedChunk(const tr::Request& r, std::uint32_t crc) const
    {
        for (std::size_t i{ 0 }; i < m_chunkCount; ++i) {
            const auto& c{ m_chunks[i] };
            if (c.branch == r.branch && c.index == r.index) {
                return c.count == r.count && c.crc == crc && c.final == r.final ? tr::Error::None
                                                                                : tr::Error::Order;
            }
        }
        return std::nullopt;
    }
    tr::Error TrajectorySession::upload(const tr::Request& r)
    {
        if (!r.count || r.count > tr::UPLOAD_SEGMENTS || r.branch > tr::Branch::Stopping) {
            return tr::Error::Protocol;
        }
        if (r.revision != m_revision + 1) {
            return tr::Error::Stale;
        }
        if (m_banks[m_staging].use_count() != 1) {
            return tr::Error::Busy;
        }
        const auto packet{ tr::encode(r) };
        const auto crc{ tr::checksum(std::span(packet.bytes).first(packet.size - sizeof(std::uint32_t))) };
        if (const auto repeated{ repeatedChunk(r, crc) }) {
            return *repeated;
        }
        auto& w{ *m_banks[m_staging] };
        const bool stopping{ r.branch == tr::Branch::Stopping };
        if ((stopping && (!w.continuation_final || w.stopping_final)) ||
            (!stopping && w.continuation_final)) {
            return tr::Error::Order;
        }
        if (r.index != (stopping ? w.stopping : w.continuation)) {
            return tr::Error::Order;
        }
        const std::size_t first{ std::size_t{ w.continuation } + w.stopping };
        if (first + r.count > w.segments.size() || m_chunkCount == m_chunks.size()) {
            return tr::Error::Capacity;
        }
        const auto hardware{ m_executor->snapshot().capabilities };
        const auto validated_ticks{ validateChunk(r, hardware) };
        if (!validated_ticks) {
            return validated_ticks.error();
        }
        const auto added_ticks{ *validated_ticks };
        if (!stopping && w.continuation_ticks + added_ticks > tr::CHECKPOINT_TICKS) {
            return tr::Error::Timing;
        }
        if (stopping && r.final && !tr::at_rest(r.segments[r.count - 1].at(1))) {
            return tr::Error::NoStoppingPath;
        }
        std::copy_n(r.segments.begin(), r.count, w.segments.begin() + static_cast<std::ptrdiff_t>(first));
        if (stopping) {
            w.stopping += r.count;
            w.stopping_final = r.final;
        }
        else {
            w.continuation += r.count;
            w.continuation_ticks += added_ticks;
            w.continuation_final = r.final;
        }
        w.total_ticks += added_ticks;
        w.revision = r.revision;
        m_chunks[m_chunkCount++] = {
            .index = r.index, .crc = crc, .count = r.count, .branch = r.branch, .final = r.final
        };
        return tr::Error::None;
    }
    tr::Error TrajectorySession::commit(const tr::Request& r)
    {
        if (r.revision != m_revision + 1) {
            return tr::Error::Stale;
        }
        auto& w{ *m_banks[m_staging] };
        if (!w.continuation || !w.continuation_final) {
            return tr::Error::Order;
        }
        if (w.stopping ? !w.stopping_final : !tr::at_rest(w.nominal().back().at(1))) {
            return tr::Error::NoStoppingPath;
        }
        // The other bank must already have been retired by the executor. An
        // incomplete transfer never displaces a live fallback stopping path.
        const auto other{ 1U - m_staging };
        if (m_banks[other].use_count() != 1) {
            return tr::Error::Busy;
        }
        const auto error{ m_executor->commit(m_banks[m_staging], r) };
        if (error != tr::Error::None) {
            return error;
        }
        m_revision = r.revision;
        m_staging = other;
        m_chunkCount = 0;
        auto& next{ *m_banks[m_staging] };
        next.continuation = next.stopping = 0;
        next.continuation_ticks = next.total_ticks = next.revision = 0;
        next.continuation_final = next.stopping_final = false;
        return tr::Error::None;
    }
}
