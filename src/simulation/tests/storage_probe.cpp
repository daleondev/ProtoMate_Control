#include "runtime/libc/filesystem.hpp"
#include "simulation/Client.hpp"
#include <array>
#include <cstdio>
#include <fcntl.h>
#include <string_view>
extern "C" int _open(const char*, int, ...);
extern "C" int _close(int);
extern "C" int _read(int, char*, int);
extern "C" int _write(int, const char*, int);
extern "C" int _fsync(int);
int main(int argc, char** argv)
{
    if (!simulation::client::connected())
        return 1;
    runtime_filex_initialize();
    if (!runtime::filex::initialized())
        return 2;
    constexpr std::string_view DATA{ "native raw hardware - production FileX/LevelX" };
    for (const auto* path : { "/flash/simulation.txt", "/sd/simulation.txt" }) {
        const bool write{ argc > 1 && std::string_view{ argv[1] } == "write" };
        const int fd{ _open(path, write ? (O_WRONLY | O_CREAT | O_TRUNC) : O_RDONLY, 0600) };
        if (fd < 0)
            return 3;
        if (write) {
            if (_write(fd, DATA.data(), static_cast<int>(DATA.size())) != DATA.size() || _fsync(fd))
                return 4;
        }
        else {
            std::array<char, 128> data{};
            const auto size{ _read(fd, data.data(), data.size()) };
            if (size < 0 || std::string_view{ data.data(), static_cast<std::size_t>(size) } != DATA)
                return 5;
        }
        if (_close(fd))
            return 6;
    }
    std::puts("PASS: native raw media through production FileX/LevelX");
    return 0;
}
