#include "Media.hpp"
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>
namespace simulation
{
    namespace
    {
        constexpr std::size_t BLOCK_BYTES{ 512 }, ERASE_BYTES{ 4096 };
        constexpr std::byte ERASED{ 0xFF };
        constexpr std::size_t TEMP_NAME_BYTES{ 32 };
        constexpr std::array<std::size_t, 2> SIZES{ 16UZ * 1024UZ * 1024UZ, 64UZ * 1024UZ * 1024UZ };
        bool can_program(int fd, std::uint64_t offset, std::span<const std::byte> bytes)
        {
            const auto count{ bytes.size() };

            std::array<std::byte, PAYLOAD_BYTES> original{};
            if (pread(fd, original.data(), count, static_cast<off_t>(offset)) !=
                static_cast<ssize_t>(count)) {
                return false;
            }
            for (std::size_t i{}; i < count; ++i) {
                if ((original[i] & bytes[i]) != bytes[i]) {
                    return false;
                }
            }
            return true;
        }
    }
    Media::Media(std::filesystem::path directory)
      : m_directory{ std::move(directory) }
    {
    }
    Media::~Media()
    {
        for (int fd : m_fd) {
            if (fd >= 0) {
                fsync(fd);
                close(fd);
            }
        }
    }
    void Media::open(unsigned device)
    {
        if (m_fd[device] >= 0) {
            return;
        }
        int fd{ -1 };
        bool created{};
        if (m_directory.empty()) {
            std::array<char, TEMP_NAME_BYTES> name{};
            const std::string pattern{ "/tmp/protomate-media-XXXXXX" };
            std::ranges::copy(pattern, name.begin());
            fd = mkstemp(name.data());
            if (fd >= 0) {
                unlink(name.data());
            }
            created = true;
        }
        else {
            std::filesystem::create_directories(m_directory);
            auto path{ m_directory / (device ? "sd.img" : "flash.img") };
            // POSIX open takes the creation mode through its variadic ABI.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
            fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
            created = fd >= 0;
            if (fd < 0 && errno == EEXIST) {
                // POSIX open retains its variadic signature without O_CREAT.
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
                fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
            }
        }
        if (fd < 0) {
            throw std::system_error{ errno, std::generic_category(), "open simulator media" };
        }
        if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
            close(fd);
            throw std::runtime_error{ "simulator image is already in use" };
        }
        struct stat st{};
        if (fstat(fd, &st) || (!created && !std::cmp_equal(st.st_size, SIZES[device]))) {
            close(fd);
            throw std::runtime_error{ "incorrect simulator image size" };
        }
        if (created) {
            std::array<std::byte, ERASE_BYTES> block{};
            if (!device) {
                block.fill(ERASED);
            }
            for (std::size_t offset{}; offset < SIZES[device]; offset += block.size()) {
                if (pwrite(fd, block.data(), block.size(), static_cast<off_t>(offset)) !=
                    static_cast<ssize_t>(block.size())) {
                    close(fd);
                    throw std::runtime_error{ "initialize simulator media" };
                }
            }
        }
        m_fd[device] = fd;
        m_created[device] = created;
    }
    void Media::write(const Packet& q, Packet& r)
    {
        const auto device{ q.arguments[0] };
        const auto offset{ q.arguments[1] };
        const auto count{ q.arguments[2] };
        auto bytes{ q.data };
        if (q.operation == Operation::MediaErase) {
            bytes.fill(ERASED);
        }
        else {
            if (q.size != count) {
                r.status = EINVAL;
                return;
            }
            if (!device && !can_program(m_fd[device], offset, std::span{ bytes }.first(count))) {
                r.status = EIO;
                return;
            }
        }
        if (pwrite(m_fd[device], bytes.data(), count, static_cast<off_t>(offset)) !=
            static_cast<ssize_t>(count)) {
            r.status = EIO;
        }
    }
    void Media::execute(const Packet& q, Packet& r)
    {
        const auto device{ q.arguments[0] };
        const auto offset{ q.arguments[1] };
        const auto count{ q.arguments[2] };
        if (device > 1) {
            r.status = EINVAL;
            return;
        }
        open(static_cast<unsigned>(device));
        if (q.operation == Operation::MediaInfo) {
            r.arguments = {
                SIZES[device], m_created[device], BLOCK_BYTES, device ? BLOCK_BYTES : ERASE_BYTES
            };
            return;
        }
        if (q.operation == Operation::MediaFlush) {
            if (fsync(m_fd[device])) {
                r.status = EIO;
            }
            return;
        }
        if (count > PAYLOAD_BYTES || offset > SIZES[device] || count > SIZES[device] - offset) {
            r.status = EINVAL;
            return;
        }
        if (q.operation == Operation::MediaRead) {
            if (pread(m_fd[device], r.data.data(), count, static_cast<off_t>(offset)) !=
                static_cast<ssize_t>(count)) {
                r.status = EIO;
            }
            else {
                r.size = static_cast<std::uint32_t>(count);
            }
        }
        else {
            write(q, r);
        }
    }
}
