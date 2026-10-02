#pragma once

#include "libc/filesystem.hpp"
#include "libc/storage_media.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

extern "C" int _open(const char*, int, ...);
extern "C" int _close(int);
extern "C" int _read(int, char*, int);
extern "C" int _write(int, const char*, int);
extern "C" off_t _lseek(int, off_t, int);
extern "C" int _fstat(int, struct stat*);
extern "C" int _stat(const char*, struct stat*);
extern "C" int _unlink(const char*);
extern "C" int _rename(const char*, const char*);
extern "C" int _mkdir(const char*, mode_t);
extern "C" int _rmdir(const char*);
extern "C" int _ftruncate(int, off_t);
extern "C" int _fsync(int);
extern "C" int _utime(const char*, const struct utimbuf*);
extern "C" int _chdir(const char*);
extern "C" char* _getcwd(char*, std::size_t);

#define STORAGE_CHECK(expression) do { if (!(expression)) { \
    std::printf("[storage-check] %s:%d errno=%d: %s\n", __FILE__, __LINE__, errno, #expression); \
    return __LINE__; } } while (false)
#define STORAGE_NO_DEVICE(expression) do { errno = 0; STORAGE_CHECK((expression) == -1 && errno == ENODEV); } while (false)

namespace runtime::tests
{
    inline auto check_storage_startup_adapters(bool flash, bool sd) -> int
    {
        STORAGE_CHECK(runtime::filex::initialized() == (flash || sd));
        STORAGE_CHECK(runtime::storage::mounted(runtime::storage::Volume::flash) == flash);
        STORAGE_CHECK(runtime::storage::mounted(runtime::storage::Volume::sd) == sd);
        std::array<unsigned char, sizeof(runtime::storage::Diagnostics)> before{};
        std::memcpy(before.data(), &runtime::storage::diagnostics(), before.size());
        runtime_filex_initialize();
        STORAGE_CHECK(runtime::storage::initialize() == (flash || sd));
        STORAGE_CHECK(std::memcmp(before.data(), &runtime::storage::diagnostics(), before.size()) == 0);
        struct stat info{};
        constexpr char payload[]{ "storage survives optional initialization" };
        constexpr utimbuf timestamp{ 1'704'164'646, 1'704'164'646 };
        for (unsigned volume{}; volume < 2U; ++volume) {
            const bool available{ volume == 0U ? flash : sd };
            const char* const mount{ volume == 0U ? "/flash" : "/sd" };
            std::array<char, 96> file{}, renamed{}, directory{};
            std::snprintf(file.data(), file.size(), "%s/nucleo-storage-startup-adapter.tmp", mount);
            std::snprintf(renamed.data(), renamed.size(), "%s/nucleo-storage-startup-adapter.renamed", mount);
            std::snprintf(directory.data(), directory.size(), "%s/nucleo-storage-startup-adapter.dir", mount);
            if (!available) {
                STORAGE_NO_DEVICE(_open(file.data(), O_CREAT | O_RDWR, 0600));
                STORAGE_NO_DEVICE(_stat(mount, &info));
                STORAGE_NO_DEVICE(_unlink(file.data()));
                STORAGE_NO_DEVICE(_rename(file.data(), renamed.data()));
                STORAGE_NO_DEVICE(_mkdir(directory.data(), 0700));
                STORAGE_NO_DEVICE(_rmdir(directory.data()));
                STORAGE_NO_DEVICE(_utime(file.data(), &timestamp));
                STORAGE_NO_DEVICE(_chdir(mount));
                std::uint64_t bytes{};
                STORAGE_CHECK(runtime::filex::availableSpace(mount, bytes) == ENODEV);
                continue;
            }
            // Refuse existing fixtures; never replace another file.
            errno = 0;
            STORAGE_CHECK(_stat(renamed.data(), &info) == -1 && errno == ENOENT);
            const int descriptor{ _open(file.data(), O_CREAT | O_EXCL | O_RDWR, 0600) };
            STORAGE_CHECK(descriptor >= 3);
            STORAGE_CHECK(_write(descriptor, payload, sizeof(payload)) == sizeof(payload));
            STORAGE_CHECK(_fsync(descriptor) == 0);
            STORAGE_CHECK(_lseek(descriptor, 0, SEEK_SET) == 0);
            std::array<char, sizeof(payload)> read{};
            STORAGE_CHECK(_read(descriptor, read.data(), read.size()) == read.size());
            STORAGE_CHECK(std::memcmp(read.data(), payload, read.size()) == 0);
            STORAGE_CHECK(_fstat(descriptor, &info) == 0);
            STORAGE_CHECK(_ftruncate(descriptor, sizeof(payload) - 1U) == 0);
            STORAGE_CHECK(_close(descriptor) == 0);
            STORAGE_CHECK(_utime(file.data(), &timestamp) == 0);
            STORAGE_CHECK(_rename(file.data(), renamed.data()) == 0);
            STORAGE_CHECK(_unlink(renamed.data()) == 0);
            STORAGE_CHECK(_mkdir(directory.data(), 0700) == 0);
            STORAGE_CHECK(_rmdir(directory.data()) == 0);
        }
        if (!flash && !sd) {
            char path[256]{};
            errno = 0;
            STORAGE_CHECK(_getcwd(path, sizeof(path)) == nullptr && errno == ENODEV);
            STORAGE_CHECK(runtime::filex::currentPath(path) == ENODEV);
            STORAGE_NO_DEVICE(_stat("/", &info));
            STORAGE_NO_DEVICE(_open("/", O_RDONLY));
            STORAGE_NO_DEVICE(_read(999, path, 1));
            STORAGE_NO_DEVICE(_write(999, "x", 1));
            STORAGE_NO_DEVICE(_lseek(999, 0, SEEK_SET));
            STORAGE_NO_DEVICE(_close(999));
            STORAGE_NO_DEVICE(_fstat(999, &info));
            STORAGE_NO_DEVICE(_ftruncate(999, 0));
            STORAGE_NO_DEVICE(_fsync(999));
        }
        else {
            char path[256]{};
            STORAGE_CHECK(runtime::filex::currentPath(path) == 0);
            STORAGE_CHECK(std::strcmp(path, flash ? "/flash" : "/sd") == 0);
        }
        for (int descriptor{}; descriptor <= 2; ++descriptor) {
            STORAGE_CHECK(_fstat(descriptor, &info) == 0 && S_ISCHR(info.st_mode));
            STORAGE_CHECK(_close(descriptor) == 0);
        }
        STORAGE_CHECK(_write(1, "[storage-console-out]\n", 22) == 22);
        STORAGE_CHECK(_write(2, "[storage-console-err]\n", 22) == 22);
        return 0;
    }
}
