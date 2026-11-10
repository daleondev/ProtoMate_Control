#include "SharedMemory.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <new>
#include <poll.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>

namespace simulation
{
    QueueResult enqueue(Ring& ring, std::span<const std::byte> frame) noexcept
    {
        if (frame.size() > FRAME_BYTES) {
            return QueueResult::TooLarge;
        }
        const auto written{ acquire(ring.written) };
        const auto read{ acquire(ring.read) };
        if (written - read > QUEUE_CAPACITY) {
            return QueueResult::Corrupt;
        }
        if (written - read == QUEUE_CAPACITY) {
            return QueueResult::Full;
        }
        auto& slot{ ring.slots[written % QUEUE_CAPACITY] };
        std::ranges::copy(frame, slot.bytes.begin());
        slot.size = static_cast<std::uint32_t>(frame.size());
        slot.checksum = checksum(frame);
        release(ring.written, written + 1U);
        return QueueResult::Success;
    }
    QueueResult dequeue(Ring& ring, std::span<std::byte> buffer, std::size_t& size) noexcept
    {
        const auto written{ acquire(ring.written) };
        const auto read{ acquire(ring.read) };
        if (written - read > QUEUE_CAPACITY) {
            return QueueResult::Corrupt;
        }
        if (written == read) {
            return QueueResult::Empty;
        }
        const auto& slot{ ring.slots[read % QUEUE_CAPACITY] };
        if (slot.size > FRAME_BYTES) {
            return QueueResult::Corrupt;
        }
        if (slot.size > buffer.size()) {
            return QueueResult::BufferTooSmall;
        }
        const auto bytes{ std::span{ slot.bytes }.first(slot.size) };
        if (checksum(bytes) != slot.checksum) {
            return QueueResult::Corrupt;
        }
        std::ranges::copy(bytes, buffer.begin());
        size = slot.size;
        release(ring.read, read + 1U);
        return QueueResult::Success;
    }
    std::uint64_t host_now() noexcept
    {
        timespec t{};
        clock_gettime(CLOCK_MONOTONIC, &t);
        return (static_cast<std::uint64_t>(t.tv_sec) * HOST_NS_PER_SECOND) +
               static_cast<std::uint64_t>(t.tv_nsec);
    }
    void notify(int fd) noexcept
    {
        const std::uint64_t value{ 1 };
        if (fd >= 0) {
            // POSIX/Linux variadic ABI with the argument types required by this operation.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
            static_cast<void>(syscall(SYS_write, fd, &value, sizeof(value)));
        }
    }
    bool wait_notification(int fd, std::chrono::milliseconds timeout) noexcept
    {
        const auto milliseconds{ static_cast<int>(timeout.count()) };
        pollfd item{ .fd = fd, .events = POLLIN, .revents = 0 };
        int result{ poll(&item, 1, milliseconds) };
        while (result < 0 && errno == EINTR) {
            result = poll(&item, 1, milliseconds);
        }
        if (result <= 0) {
            return false;
        }
        std::uint64_t value{};
        // POSIX/Linux variadic ABI with the argument types required by this operation.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        static_cast<void>(syscall(SYS_read, fd, &value, sizeof(value)));
        return true;
    }
    Mapping::Mapping()
      : m_fd{ static_cast<int>(
          // POSIX/Linux variadic ABI with the argument types required by this operation.
          // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
          syscall(SYS_memfd_create, "protomate-simulation", MFD_CLOEXEC | MFD_ALLOW_SEALING)) }
    {

        if (m_fd < 0) {
            throw std::runtime_error{ "memfd_create failed" };
        }
        // POSIX/Linux variadic ABI with the argument types required by this operation.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        if (syscall(SYS_ftruncate, m_fd, MAPPING_BYTES) != 0) {
            // POSIX/Linux variadic ABI with the argument types required by this operation.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
            syscall(SYS_close, m_fd);
            throw std::runtime_error{ "memfd resize failed" };
        }
        auto* memory{ mmap(nullptr, MAPPING_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0) };
        if (memory == MAP_FAILED) {
            // POSIX/Linux variadic ABI with the argument types required by this operation.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
            syscall(SYS_close, m_fd);
            throw std::runtime_error{ "simulation mmap failed" };
        }
        m_memory = std::construct_at(static_cast<SharedMemory*>(memory));
        m_memory->magic = MAGIC;
        m_memory->version = VERSION;
        m_memory->bytes = MAPPING_BYTES;
        if (getrandom(&m_memory->session, sizeof(m_memory->session), 0) != sizeof(m_memory->session)) {
            m_memory->session = host_now();
        }
        // POSIX/Linux variadic ABI with the argument types required by this operation.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        if (fcntl(m_fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) != 0) {
            munmap(memory, MAPPING_BYTES);
            // POSIX/Linux variadic ABI with the argument types required by this operation.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
            syscall(SYS_close, m_fd);
            throw std::runtime_error{ "simulation sealing failed" };
        }
    }
    Mapping::Mapping(int fd)
    {
        // POSIX/Linux variadic ABI with the argument types required by this operation.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        const int seals{ fcntl(fd, F_GET_SEALS) };
        constexpr int required{ F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL };
        if (seals < 0 || (seals & required) != required) {
            throw std::runtime_error{ "unsealed simulation mapping" };
        }
        struct stat information{};
        // POSIX/Linux variadic ABI with the argument types required by this operation.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        if (syscall(SYS_fstat, fd, &information) != 0 ||
            std::cmp_not_equal(information.st_size, MAPPING_BYTES)) {
            throw std::runtime_error{ "invalid simulation mapping size" };
        }
        // POSIX/Linux variadic ABI with the argument types required by this operation.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
        m_fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
        if (m_fd < 0) {
            throw std::runtime_error{ "cannot duplicate simulation descriptor" };
        }
        auto* memory{ mmap(nullptr, MAPPING_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0) };
        if (memory == MAP_FAILED) {
            // POSIX/Linux variadic ABI with the argument types required by this operation.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
            syscall(SYS_close, m_fd);
            throw std::runtime_error{ "simulation mmap failed" };
        }
        m_memory = static_cast<SharedMemory*>(memory);
        if (m_memory->magic != MAGIC || m_memory->version != VERSION || m_memory->bytes != MAPPING_BYTES) {
            munmap(memory, MAPPING_BYTES);
            // POSIX/Linux variadic ABI with the argument types required by this operation.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
            syscall(SYS_close, m_fd);
            throw std::runtime_error{ "incompatible simulation mapping" };
        }
    }
    Mapping::~Mapping()
    {
        if (m_memory) {
            munmap(m_memory, MAPPING_BYTES);
        }
        if (m_fd >= 0) {
            // POSIX/Linux variadic ABI with the argument types required by this operation.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
            syscall(SYS_close, m_fd);
        }
    }
    Mapping::Mapping(Mapping&& other) noexcept
      : m_fd{ std::exchange(other.m_fd, -1) }
      , m_memory{ std::exchange(other.m_memory, nullptr) }
    {
    }
}
