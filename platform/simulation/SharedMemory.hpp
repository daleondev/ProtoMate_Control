#pragma once
#include "Protocol.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <utility>

namespace simulation
{
    inline constexpr std::size_t MAPPING_BYTES{ static_cast<std::size_t>(512U) * 1024U }, QUEUE_CAPACITY{ 8 },
      BULK_COUNT{ 32 }, BULK_BYTES{ 4096 };
    inline constexpr std::size_t CACHE_LINE_BYTES{ 64 }, GPIO_COUNT{ 176 }, STEP_SAMPLE_WORDS{ 32 },
      TIMER_COUNT{ 18 }, TIMER_FIELDS{ 8 }, PWM_COUNT{ 3 };
    inline constexpr std::uint64_t HOST_NS_PER_SECOND{ 1'000'000'000ULL };
    inline constexpr std::uint32_t MAGIC{ 0x504D5349U };
    struct Slot
    {
        std::uint32_t size{}, checksum{};
        std::array<std::byte, FRAME_BYTES> bytes{};
    };
    struct alignas(CACHE_LINE_BYTES) Ring
    {
        alignas(CACHE_LINE_BYTES) std::uint32_t written{};
        alignas(CACHE_LINE_BYTES) std::uint32_t read{};
        std::array<Slot, QUEUE_CAPACITY> slots{};
    };
    // Shared atomics/rings deliberately occupy separate cache lines. This is a process ABI.
    // NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
    struct alignas(CACHE_LINE_BYTES) SharedMemory
    {
        std::uint32_t magic{}, version{}, bytes{}, reserved{};
        std::uint64_t session{};
        alignas(CACHE_LINE_BYTES) std::uint64_t time_ns{}, server_heartbeat{}, client_heartbeat{},
          encoder_count{}, epoch_ns{};
        alignas(CACHE_LINE_BYTES) std::uint32_t closed{}, irq_sequence{}, irq_ack{}, irq_flags{},
          client_ready{}, idle_epoch{};
        // idle_epoch = irq_sequence + 1 after all runnable work has suspended.
        // Its initial zero cannot authorize the first advance (sequence zero).
        std::array<std::uint32_t, GPIO_COUNT> gpio{};
        std::array<std::uint32_t, STEP_SAMPLE_WORDS> step_sample{};
        std::array<std::array<std::uint32_t, TIMER_FIELDS>, TIMER_COUNT> timers{};
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
    enum class QueueResult : std::uint8_t
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
    std::uint64_t host_now() noexcept;
    void notify(int fd) noexcept;
    bool wait_notification(int fd, std::chrono::milliseconds timeout) noexcept;

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
