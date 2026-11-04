#include "Media.hpp"
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
namespace simulation
{
    namespace
    {
        constexpr std::array<std::size_t, 2> sizes{ 16U * 1024U * 1024U, 64U * 1024U * 1024U };
    }
    Media::Media(const std::filesystem::path& directory)
      : m_directory{ directory }
    {
    }
    Media::~Media()
    {
        for (int fd : m_fd)
            if (fd >= 0) {
                fsync(fd);
                close(fd);
            }
    }
    void Media::open(unsigned device)
    {
        if (m_fd[device] >= 0)
            return;
        int fd{ -1 };
        bool created{};
        if (m_directory.empty()) {
            std::array<char, 32> name{};
            const std::string pattern{ "/tmp/protomate-media-XXXXXX" };
            std::copy(pattern.begin(), pattern.end(), name.begin());
            fd = mkstemp(name.data());
            if (fd >= 0)
                unlink(name.data());
            created = true;
        }
        else {
            std::filesystem::create_directories(m_directory);
            auto path{ m_directory / (device ? "sd.img" : "flash.img") };
            fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            created = fd >= 0;
            if (fd < 0 && errno == EEXIST)
                fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        }
        if (fd < 0)
            throw std::system_error{ errno, std::generic_category(), "open simulator media" };
        if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
            close(fd);
            throw std::runtime_error{ "simulator image is already in use" };
        }
        struct stat st{};
        if (fstat(fd, &st) || (!created && static_cast<std::size_t>(st.st_size) != sizes[device])) {
            close(fd);
            throw std::runtime_error{ "incorrect simulator image size" };
        }
        if (created) {
            std::array<std::byte, 4096> block{};
            if (!device)
                block.fill(std::byte{ 255 });
            for (std::size_t offset{}; offset < sizes[device]; offset += block.size())
                if (pwrite(fd, block.data(), block.size(), static_cast<off_t>(offset)) !=
                    static_cast<ssize_t>(block.size())) {
                    close(fd);
                    throw std::runtime_error{ "initialize simulator media" };
                }
        }
        m_fd[device] = fd;
        m_created[device] = created;
    }
    void Media::execute(const Packet& q, Packet& r)
    {
        const auto device{ q.arguments[0] }, offset{ q.arguments[1] }, count{ q.arguments[2] };
        if (device > 1) {
            r.status = EINVAL;
            return;
        }
        open(static_cast<unsigned>(device));
        if (q.operation == Operation::MediaInfo) {
            r.arguments = { sizes[device], m_created[device], 512, device ? 512U : 4096U };
            return;
        }
        if (q.operation == Operation::MediaFlush) {
            if (fsync(m_fd[device]))
                r.status = EIO;
            return;
        }
        if (count > PAYLOAD_BYTES || offset > sizes[device] || count > sizes[device] - offset) {
            r.status = EINVAL;
            return;
        }
        if (q.operation == Operation::MediaRead) {
            if (pread(m_fd[device], r.data.data(), count, static_cast<off_t>(offset)) !=
                static_cast<ssize_t>(count))
                r.status = EIO;
            else
                r.size = static_cast<std::uint32_t>(count);
        }
        else {
            auto bytes{ q.data };
            if (q.operation == Operation::MediaErase)
                bytes.fill(std::byte{ 255 });
            else {
                if (q.size != count) {
                    r.status = EINVAL;
                    return;
                }
                if (!device) {
                    std::array<std::byte, PAYLOAD_BYTES> original{};
                    if (pread(m_fd[device], original.data(), count, static_cast<off_t>(offset)) !=
                        static_cast<ssize_t>(count)) {
                        r.status = EIO;
                        return;
                    }
                    for (std::size_t i{}; i < count; ++i)
                        if ((original[i] & bytes[i]) != bytes[i]) {
                            r.status = EIO;
                            return;
                        }
                }
            }
            if (pwrite(m_fd[device], bytes.data(), count, static_cast<off_t>(offset)) !=
                static_cast<ssize_t>(count))
                r.status = EIO;
        }
    }
}
