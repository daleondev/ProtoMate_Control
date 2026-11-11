#include "Services.hpp"
#include <algorithm>
#include <atomic>
#include <new>
#include <stdexcept>

namespace ui
{
    namespace
    {
#if defined(CORE_CM7)
        alignas(32) __attribute__((section(".ui_buffers.services"))) std::array<std::byte, 16384> storage;
        std::atomic_bool claimed{};
#endif
    }
    auto Services::reserveStorage() -> std::unique_ptr<Storage, ReleaseStorage>
    {
#if defined(CORE_CM7)
        static_assert(sizeof(Storage) <= sizeof(storage));
        if (claimed.exchange(true)) throw std::logic_error("Application services already owned");
        return std::unique_ptr<Storage, ReleaseStorage>{ std::construct_at(reinterpret_cast<Storage*>(storage.data())) };
#else
        return std::unique_ptr<Storage, ReleaseStorage>{ new Storage{} };
#endif
    }
    void Services::ReleaseStorage::operator()(Storage* value) const noexcept
    {
#if defined(CORE_CM7)
        std::destroy_at(value);
        claimed = false;
#else
        delete value;
#endif
    }
    std::uint64_t Services::reserveOperation(std::uint64_t source, bool urgent)
    {
        std::scoped_lock lock{ m_mutex };
        std::span<Operation> slots{ m_storage->operations };
        slots = urgent ? slots.subspan(16) : slots.first(16);
        Operation* available{};
        for (auto& slot : slots) {
            if (!slot.pending && (!available || slot.value.operation < available->value.operation)) available = &slot;
        }
        if (!available || m_next == UINT64_MAX) { ++m_operationRejections; return 0; }
        *available = {};
        available->source = source;
        available->pending = true;
        available->value.operation = m_next++;
        m_pendingPeak = std::max<unsigned>(m_pendingPeak, std::ranges::count_if(m_storage->operations, [](const auto& operation) {return operation.pending;}));
        return available->value.operation;
    }
    void Services::publishOutcome(services::Outcome value, bool pending)
    {
        if (!value.operation) return; // Uncorrelated diagnostic refresh has no operation slot.
        {
            std::scoped_lock lock{ m_mutex };
            auto slot{ std::ranges::find_if(m_storage->operations, [&](const auto& entry) {
                return entry.value.operation == value.operation;
            }) };
            if (slot == m_storage->operations.end()) return;
            // A native completion may arrive before submit() returns. Never
            // replace that completion with its older submission receipt.
            if (slot->value.phase != services::Outcome::Phase::Submitted) return;
            value.sequence = ++m_outcomeSequence;
            slot->value = value;
            slot->pending = pending;
        }
        message(value.message.data(), value.severity, value.operation);
        notifySubscribers();
    }
    std::optional<services::Outcome> Services::operation(std::uint64_t id, std::uint64_t source) const
    {
        std::scoped_lock lock{ m_mutex };
        for (const auto& entry : m_storage->operations)
            if (entry.value.operation == id && entry.source == source && id) return entry.value;
        return {};
    }
    std::optional<services::Outcome> Services::nextOutcome(std::uint64_t after, std::uint64_t source) const
    {
        std::scoped_lock lock{ m_mutex };
        const services::Outcome* next{};
        for (const auto& entry : m_storage->operations) {
            if (entry.source == source && entry.value.sequence > after &&
                (!next || entry.value.sequence < next->sequence)) next = &entry.value;
        }
        return next ? std::optional{ *next } : std::nullopt;
    }
    bool Services::subscribe(const std::shared_ptr<runtime::Notification>& notification)
    {
        if (!notification) return false;
        std::scoped_lock lock{ m_mutex };
        for (const auto& slot : m_subscribers) if (slot.lock() == notification) return true;
        for (auto& slot : m_subscribers) if (slot.expired()) { slot = notification; return true; }
        return false;
    }
    void Services::notifySubscribers()
    {
        std::array<std::shared_ptr<runtime::Notification>, 4> listeners;
        {
            std::scoped_lock lock{ m_mutex };
            for (unsigned i{}; i < listeners.size(); ++i) listeners[i] = m_subscribers[i].lock();
        }
        for (const auto& listener : listeners) if (listener) listener->signal();
    }
    void Services::message(const char* value, services::Severity severity, std::uint64_t operation)
    {
        if (!value || !*value) return;
        {
            std::scoped_lock lock{m_mutex};
            auto& rows{m_storage->messages};
            // A GUI presentation of an already recorded operation is not a
            // second event. Truncate before comparing, consistently across frontends.
            services::Message next{};
            std::size_t count{};
            while (value[count] && count + 1 < next.text.size()) {next.text[count] = value[count]; ++count;}
            if (m_messageSequence) {
                const auto& last{rows[(m_messageSequence - 1) % rows.size()]};
                if (last.text == next.text && last.severity == severity) return;
            }
            next.sequence = ++m_messageSequence;
            next.time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            next.operation = operation; next.severity = severity;
            rows[(next.sequence - 1) % rows.size()] = next;
            ++m_messageRevision;
        }
        notifySubscribers();
    }
    unsigned Services::messages(unsigned offset, std::span<services::Message> output, std::uint64_t* revision) const
    {
        std::scoped_lock lock{m_mutex};
        if (revision) *revision = m_messageRevision;
        const auto& rows{m_storage->messages};
        const auto count{std::min<std::uint64_t>(m_messageSequence, rows.size())};
        unsigned written{};
        while (offset < count && written < output.size())
            output[written++] = rows[(m_messageSequence - 1 - offset++) % rows.size()];
        return written;
    }
    std::uint64_t Services::messageRevision() const
    { std::scoped_lock lock{m_mutex}; return m_messageRevision; }
    void Services::acknowledgeMessages(std::uint64_t through)
    {
        {
            std::scoped_lock lock{m_mutex};
            for (auto& row : m_storage->messages)
                if (row.sequence && row.sequence <= through) row.acknowledged = true;
            ++m_messageRevision;
        }
        notifySubscribers();
    }
    bool Services::refreshDiagnostics(bool io, bool stacks)
    {
        using enum services::Kind;
        try {requestMemory({.kind = io ? (stacks ? MemoryIOStack : MemoryIO) : (stacks ? MemoryUIStack : MemoryUI)}, 0);}
        catch (const std::exception&) {return false;}
        return true;
    }
    void Services::graphicsHooks(std::function<services::Graphics()> read, std::function<bool(unsigned)> brightness)
    {m_graphics = std::move(read); m_brightness = std::move(brightness);}
    services::Graphics Services::graphics() const {return m_graphics ? m_graphics() : services::Graphics{};}
    services::QueueDiagnostics Services::queues() const
    {
        std::scoped_lock lock{m_mutex};
        services::QueueDiagnostics out{.peak = m_pendingPeak, .rejected = m_operationRejections};
        for (unsigned i{}; i < m_storage->operations.size(); ++i) {
            if (m_storage->operations[i].pending) {
                if (i < 16) ++out.pending; else ++out.urgent;
            }
        }
        return out;
    }
}
