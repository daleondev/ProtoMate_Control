#pragma once

#include <array>
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>

// Newlib omits this declaration for the bare-metal target; the FileX
// adapter supplies the POSIX entry point.
extern "C" int ftruncate(int file, off_t length);

namespace runtime::tests
{
    [[nodiscard]] inline auto check_storage_timestamps(const char* path) -> bool
    {
        namespace fs = std::filesystem;
        using namespace std::chrono;
        const int file{ ::open(path, O_CREAT | O_RDWR | O_TRUNC, 0666) };
        if (file < 0) {
            return false;
        }
        const fs::path directory{ std::string{ path } + ".dir" };
        const auto requested{ fs::file_time_type::clock::from_sys(sys_days{ year{ 2024 } / February / 29 } +
                                                                  12h + 34min + 57s + 750ms) };
        const auto rounded{ fs::file_time_type::clock::from_sys(sys_days{ year{ 2024 } / February / 29 } +
                                                                12h + 34min + 56s) };
        std::error_code error;
        const bool passed{ [&] {
            if (::write(file, "abc", 3U) != 3) {
                return false;
            }
            fs::last_write_time(path, requested, error);
            if (error || fs::last_write_time(path, error) != rounded || error ||
                ::lseek(file, 0, SEEK_CUR) != 3 || !fs::create_directory(directory, error) || error) {
                return false;
            }
            // Exercise the throwing overload as well as directory timestamps.
            fs::last_write_time(directory, requested);
            if (fs::last_write_time(directory) != rounded) {
                return false;
            }
            const auto invalid{ fs::file_time_type::clock::from_sys(
              sys_days{ year{ 1979 } / December / 31 }) };
            fs::last_write_time(path, invalid, error);
            if (error != std::errc::value_too_large) {
                return false;
            }
            fs::last_write_time(directory / "missing", requested, error);
            return error == std::errc::no_such_file_or_directory;
        }() };
        const bool closed{ ::close(file) == 0 };
        error.clear();
        const bool preserved{ fs::last_write_time(path, error) == rounded && !error };
        static_cast<void>(fs::remove(directory, error));
        const bool directory_removed{ !error };
        const bool removed{ fs::remove(path, error) && !error };
        return passed && closed && preserved && directory_removed && removed;
    }

    [[nodiscard]] inline auto check_storage_reader_visibility(const char* path) -> bool
    {
        const int placeholder{ ::open(path, O_CREAT | O_RDONLY, 0666) };
        if (placeholder < 0) {
            return false;
        }
        const int writer{ ::open(path, O_WRONLY | O_TRUNC) };
        const bool placeholder_closed{ ::close(placeholder) == 0 };
        if (writer < 0) {
            static_cast<void>(::unlink(path));
            return false;
        }
        int reader{ -1 };
        const bool passed{ [&] {
            if (!placeholder_closed || ::write(writer, "abc", 3U) != 3) {
                return false;
            }
            reader = ::open(path, O_RDONLY);
            if (reader != placeholder) {
                return false;
            }
            struct stat metadata{};
            std::array<char, 3U> result{};
            const std::array<char, 3U> expected{ 'a', 'b', 'c' };
            return ::stat(path, &metadata) == 0 && metadata.st_size == 3 && ::fstat(reader, &metadata) == 0 &&
                   metadata.st_size == 3 &&
                   ::read(reader, result.data(), result.size()) == static_cast<ssize_t>(result.size()) &&
                   result == expected && ::ftruncate(writer, 1) == 0 && ::stat(path, &metadata) == 0 &&
                   metadata.st_size == 1 && ::fstat(reader, &metadata) == 0 && metadata.st_size == 1;
        }() };
        const bool reader_closed{ reader < 0 || ::close(reader) == 0 };
        const bool writer_closed{ ::close(writer) == 0 };
        const bool removed{ ::unlink(path) == 0 };
        return passed && reader_closed && writer_closed && removed;
    }

    // The caller supplies a disposable test file on either mounted volume.
    [[nodiscard]] inline auto check_storage_offsets(const char* path) -> bool
    {
        const int file{ ::open(path, O_CREAT | O_RDWR | O_TRUNC, 0666) };
        if (file < 0) {
            return false;
        }
        const bool passed{ [&] {
            char unused{};
            struct stat metadata{};
            if (::write(file, "abc", 3U) != 3 || ::lseek(file, 8, SEEK_SET) != 8 ||
                ::read(file, &unused, 1U) != 0 || ::lseek(file, 0, SEEK_CUR) != 8 ||
                ::fstat(file, &metadata) != 0 || metadata.st_size != 3 || ::write(file, "z", 1U) != 1 ||
                ::lseek(file, 0, SEEK_SET) != 0) {
                return false;
            }
            std::array<char, 9U> initial{};
            const std::array<char, 9U> expected_initial{ 'a', 'b', 'c', 0, 0, 0, 0, 0, 'z' };
            if (::read(file, initial.data(), initial.size()) != static_cast<ssize_t>(initial.size()) ||
                initial != expected_initial || ::ftruncate(file, 2) != 0 || ::lseek(file, 0, SEEK_CUR) != 9 ||
                ::stat(path, &metadata) != 0 || metadata.st_size != 2 || ::write(file, "q", 1U) != 1 ||
                ::lseek(file, 0, SEEK_SET) != 0) {
                return false;
            }
            std::array<char, 10U> truncated{};
            const std::array<char, 10U> expected_truncated{ 'a', 'b', 0, 0, 0, 0, 0, 0, 0, 'q' };
            return ::read(file, truncated.data(), truncated.size()) ==
                     static_cast<ssize_t>(truncated.size()) &&
                   truncated == expected_truncated && ::lseek(file, 14, SEEK_SET) == 14 &&
                   ::ftruncate(file, 12) == 0 && ::lseek(file, 0, SEEK_CUR) == 14;
        }() };
        const bool closed{ ::close(file) == 0 };
        const bool removed{ ::unlink(path) == 0 };
        return passed && closed && removed && check_storage_reader_visibility(path) &&
               check_storage_timestamps(path);
    }
}
