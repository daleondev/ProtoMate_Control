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

namespace simulation
{
    QueueResult enqueue(Ring& ring, std::span<const std::byte> frame) noexcept
    {
        if (frame.size() > FRAME_BYTES)
            return QueueResult::TooLarge;
        const auto written{ acquire(ring.written) }, read{ acquire(ring.read) };
        if (written - read > QUEUE_CAPACITY)
            return QueueResult::Corrupt;
        if (written - read == QUEUE_CAPACITY)
            return QueueResult::Full;
        auto& slot{ ring.slots[written % QUEUE_CAPACITY] };
        std::copy(frame.begin(), frame.end(), slot.bytes.begin());
        slot.size = static_cast<std::uint32_t>(frame.size());
        slot.checksum = checksum(frame);
        release(ring.written, written + 1U);
        return QueueResult::Success;
    }
    QueueResult dequeue(Ring& ring, std::span<std::byte> buffer, std::size_t& size) noexcept
    {
        const auto written{ acquire(ring.written) }, read{ acquire(ring.read) };
        if (written - read > QUEUE_CAPACITY)
            return QueueResult::Corrupt;
        if (written == read)
            return QueueResult::Empty;
        const auto& slot{ ring.slots[read % QUEUE_CAPACITY] };
        if (slot.size > FRAME_BYTES)
            return QueueResult::Corrupt;
        if (slot.size > buffer.size())
            return QueueResult::BufferTooSmall;
        const auto bytes{ std::span{ slot.bytes }.first(slot.size) };
        if (checksum(bytes) != slot.checksum)
            return QueueResult::Corrupt;
        std::copy(bytes.begin(), bytes.end(), buffer.begin());
        size = slot.size;
        release(ring.read, read + 1U);
        return QueueResult::Success;
    }
    std::uint64_t hostNow() noexcept
    {
        timespec t{};
        clock_gettime(CLOCK_MONOTONIC, &t);
        return static_cast<std::uint64_t>(t.tv_sec) * 1'000'000'000ULL +
               static_cast<std::uint64_t>(t.tv_nsec);
    }
    void notify(int fd) noexcept
    {
        const std::uint64_t value{ 1 };
        if (fd >= 0)
            static_cast<void>(syscall(SYS_write, fd, &value, sizeof(value)));
    }
    bool waitNotification(int fd, int milliseconds) noexcept
    {
        pollfd item{ fd, POLLIN, 0 };
        int result{};
        do {
            result = poll(&item, 1, milliseconds);
        } while (result < 0 && errno == EINTR);
        if (result <= 0)
            return false;
        std::uint64_t value{};
        static_cast<void>(syscall(SYS_read, fd, &value, sizeof(value)));
        return true;
    }
    Mapping::Mapping()
    {
        m_fd = static_cast<int>(
          syscall(SYS_memfd_create, "protomate-simulation", MFD_CLOEXEC | MFD_ALLOW_SEALING));
        if (m_fd < 0)
            throw std::runtime_error{ "memfd_create failed" };
        if (syscall(SYS_ftruncate, m_fd, MAPPING_BYTES) != 0) {
            syscall(SYS_close, m_fd);
            throw std::runtime_error{ "memfd resize failed" };
        }
        auto* memory{ mmap(nullptr, MAPPING_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0) };
        if (memory == MAP_FAILED) {
            syscall(SYS_close, m_fd);
            throw std::runtime_error{ "simulation mmap failed" };
        }
        m_memory = new (memory) SharedMemory{};
        m_memory->magic = MAGIC;
        m_memory->version = VERSION;
        m_memory->bytes = MAPPING_BYTES;
        if (getrandom(&m_memory->session, sizeof(m_memory->session), 0) != sizeof(m_memory->session))
            m_memory->session = hostNow();
        if (fcntl(m_fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) != 0) {
            munmap(memory, MAPPING_BYTES);
            syscall(SYS_close, m_fd);
            throw std::runtime_error{ "simulation sealing failed" };
        }
    }
    Mapping::Mapping(int fd)
    {
        struct stat information{};
        if (syscall(SYS_fstat, fd, &information) != 0 || information.st_size != MAPPING_BYTES)
            throw std::runtime_error{ "invalid simulation mapping size" };
        m_fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
        if (m_fd < 0)
            throw std::runtime_error{ "cannot duplicate simulation descriptor" };
        auto* memory{ mmap(nullptr, MAPPING_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0) };
        if (memory == MAP_FAILED) {
            syscall(SYS_close, m_fd);
            throw std::runtime_error{ "simulation mmap failed" };
        }
        m_memory = static_cast<SharedMemory*>(memory);
        if (m_memory->magic != MAGIC || m_memory->version != VERSION || m_memory->bytes != MAPPING_BYTES) {
            munmap(memory, MAPPING_BYTES);
            syscall(SYS_close, m_fd);
            throw std::runtime_error{ "incompatible simulation mapping" };
        }
    }
    Mapping::~Mapping()
    {
        if (m_memory)
            munmap(m_memory, MAPPING_BYTES);
        if (m_fd >= 0)
            syscall(SYS_close, m_fd);
    }
    Mapping::Mapping(Mapping&& other) noexcept
      : m_fd{ std::exchange(other.m_fd, -1) }
      , m_memory{ std::exchange(other.m_memory, nullptr) }
    {
    }
}
