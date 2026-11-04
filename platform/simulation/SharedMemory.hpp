#pragma once
#include "Protocol.hpp"
#include <atomic>
#include <utility>

namespace simulation
{
    inline constexpr std::size_t MAPPING_BYTES{ 512U * 1024U }, QUEUE_CAPACITY{ 8 }, BULK_COUNT{ 32 },
      BULK_BYTES{ 4096 };
    inline constexpr std::uint32_t MAGIC{ 0x504D5349U };
    struct Slot
    {
        std::uint32_t size{}, checksum{};
        std::array<std::byte, FRAME_BYTES> bytes{};
    };
    struct alignas(64) Ring
    {
        alignas(64) std::uint32_t written{};
        alignas(64) std::uint32_t read{};
        std::array<Slot, QUEUE_CAPACITY> slots{};
    };
    struct alignas(64) SharedMemory
    {
        std::uint32_t magic{}, version{}, bytes{}, reserved{};
        std::uint64_t session{};
        alignas(64) std::uint64_t time_ns{}, server_heartbeat{}, client_heartbeat{},
          encoder_count{}, epoch_ns{};
        alignas(64) std::uint32_t closed{}, irq_sequence{}, irq_ack{}, irq_flags{}, client_ready{};
        std::array<std::uint32_t, 176> gpio{};
        std::array<std::uint32_t, 32> step_sample{};
        std::array<std::array<std::uint32_t, 8>, 18> timers{};
        std::array<std::array<std::uint64_t, 3>, 3> pwm{};
        std::array<std::array<Ring, 3>, 2> incoming{};
        std::array<Ring, 2> registers{};
        std::array<std::array<std::byte, BULK_BYTES>, BULK_COUNT> bulk{};
        std::array<std::uint32_t, BULK_COUNT> bulk_generation{};
    };
    static_assert(sizeof(SharedMemory) <= MAPPING_BYTES);
    static_assert(std::atomic_ref<std::uint32_t>::is_always_lock_free);
    static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free);

    template<typename T>
    T acquire(T& value) noexcept
    {
        return std::atomic_ref<T>{ value }.load(std::memory_order_acquire);
    }
    template<typename T>
    void release(T& destination, T value) noexcept
    {
        std::atomic_ref<T>{ destination }.store(value, std::memory_order_release);
    }
    enum class QueueResult
    {
        Success,
        Empty,
        Full,
        TooLarge,
        BufferTooSmall,
        Corrupt
    };
    QueueResult enqueue(Ring& ring, std::span<const std::byte> frame) noexcept;
    QueueResult dequeue(Ring& ring, std::span<std::byte> buffer, std::size_t& size) noexcept;
    std::uint64_t hostNow() noexcept;
    void notify(int fd) noexcept;
    bool waitNotification(int fd, int milliseconds) noexcept;

    class Mapping
    {
      public:
        Mapping();
        explicit Mapping(int fd);
        ~Mapping();
        Mapping(const Mapping&) = delete;
        Mapping& operator=(const Mapping&) = delete;
        Mapping(Mapping&& other) noexcept;
        Mapping& operator=(Mapping&&) = delete;
        SharedMemory& memory() const noexcept { return *m_memory; }
        int fd() const noexcept { return m_fd; }

      private:
        int m_fd{ -1 };
        SharedMemory* m_memory{};
    };
}
