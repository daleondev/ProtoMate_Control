#include "libc/filesystem.hpp"
#include "libc/storage_media.hpp"

#include <fx_api.h>

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utime.h>
#include <vector>

extern "C" int _open(const char* path, int flags, ...);
extern "C" int _close(int file);
extern "C" int _read(int file, char* buffer, int length);
extern "C" int _write(int file, const char* buffer, int length);
extern "C" off_t _lseek(int file, off_t offset, int origin);
extern "C" int _fstat(int file, struct stat* value);
extern "C" int _stat(const char* path, struct stat* value);
extern "C" int _unlink(const char* path);
extern "C" int _rename(const char* old_path, const char* new_path);
extern "C" int _mkdir(const char* path, mode_t mode);
extern "C" int _rmdir(const char* path);
extern "C" int _ftruncate(int file, off_t length);
extern "C" int _fsync(int file);
extern "C" int _utime(const char* path, const struct utimbuf* times);

namespace
{
    std::atomic<int> console_input_calls{};
    std::atomic<int> console_input_failure_after{ -1 };
    std::atomic<int> console_input_newline_at{ -1 };
    std::atomic<int> console_output_calls{};
    std::atomic<int> console_output_failure_after{ -1 };

    class FileXTest : public ::testing::Test
    {
      protected:
        static void SetUpTestSuite() { runtime_filex_initialize(); }

        void TearDown() override
        {
            static_cast<void>(runtime::filex::setCurrentPath("/flash/"));
            static_cast<void>(_unlink("/flash/renamed.bin"));
            static_cast<void>(_unlink("/flash/mode.bin"));
            static_cast<void>(_unlink("/flash/append.bin"));
            static_cast<void>(_unlink("/flash/truncate.bin"));
            static_cast<void>(_unlink("/flash/shared.bin"));
            static_cast<void>(_unlink("/flash/durable-empty.bin"));
            static_cast<void>(_unlink("/sd/roundtrip.bin"));
            static_cast<void>(_unlink("/sd/durable-empty.bin"));
            static_cast<void>(_unlink("/flash/cross-volume.bin"));
            static_cast<void>(_unlink("/flash/seek-overflow.bin"));
            static_cast<void>(_unlink("/flash/seek-gap.bin"));
            static_cast<void>(_unlink("/flash/invalid-flags.bin"));
            static_cast<void>(_unlink("/flash/timestamp.bin"));
            static_cast<void>(_unlink("/sd/timestamp.bin"));
            static_cast<void>(_rmdir("/flash/timestamp-directory"));
            static_cast<void>(_unlink("/flash/rename-source.bin"));
            static_cast<void>(_unlink("/flash/rename-destination.bin"));
            static_cast<void>(_unlink("/flash/open-rename-source.bin"));
            static_cast<void>(_unlink("/flash/open-rename-destination.bin"));
            static_cast<void>(_unlink("/flash/rename-destination-directory/child.bin"));
            static_cast<void>(_rmdir("/flash/rename-source-directory"));
            static_cast<void>(_rmdir("/flash/rename-destination-directory"));
            static_cast<void>(_unlink("/flash/case-name.bin"));
            static_cast<void>(_unlink("/flash/mixed-target.bin"));
            static_cast<void>(_unlink("/flash/renamed-cwd/child/relative.bin"));
            static_cast<void>(_rmdir("/flash/renamed-cwd/child"));
            static_cast<void>(_rmdir("/flash/renamed-cwd"));
            static_cast<void>(_rmdir("/flash/original-cwd/child"));
            static_cast<void>(_rmdir("/flash/original-cwd"));
            static_cast<void>(_rmdir("/flash/self-tree/child/grandchild"));
            static_cast<void>(_rmdir("/flash/self-tree/child"));
            static_cast<void>(_rmdir("/flash/self-tree"));
            static_cast<void>(_rmdir("/flash/working-directory"));
            for (unsigned int index{}; index < 16U; ++index) {
                const std::string path{ "/flash/entry-history/" + std::to_string(index) + ".txt" };
                static_cast<void>(_unlink(path.c_str()));
            }
            static_cast<void>(_rmdir("/flash/entry-history"));
            static_cast<void>(_rmdir("/flash/directory"));
            static_cast<void>(_rmdir("/flash/durable-directory"));
            static_cast<void>(_rmdir("/flash/durable-renamed-directory"));
            for (unsigned index{}; index < 4U; ++index) {
                const std::string path{ "/flash/concurrent" + std::to_string(index) };
                static_cast<void>(_unlink(path.c_str()));
            }
        }
    };
}

extern "C" int __io_getchar()
{
    const int call{ console_input_calls.fetch_add(1, std::memory_order_relaxed) };
    const int failure_after{ console_input_failure_after.load(std::memory_order_relaxed) };
    const int newline_at{ console_input_newline_at.load(std::memory_order_relaxed) };
    if (failure_after >= 0 && call >= failure_after) {
        return -1;
    }
    return call == newline_at ? '\n' : 'x';
}

extern "C" int __io_putchar(int character)
{
    const int call{ console_output_calls.fetch_add(1, std::memory_order_relaxed) };
    const int failure_after{ console_output_failure_after.load(std::memory_order_relaxed) };
    return failure_after >= 0 && call >= failure_after ? -1 : character;
}

TEST_F(FileXTest, NormalizesAbsoluteAndRelativePaths)
{
    char path[runtime::filex::MAXIMUM_PATH]{};
    EXPECT_EQ(runtime::filex::normalizePath("/flash/alpha//beta/./gamma", path), 0);
    EXPECT_STREQ(path, "/flash/alpha/beta/gamma");
    EXPECT_EQ(runtime::filex::normalizePath("../../escape", path), EACCES);
}

TEST_F(FileXTest, ExposesPersistentFlashAndSdMounts)
{
    runtime::filex::EntryInformation information{};
    ASSERT_EQ(runtime::filex::information("/flash/", information), 0);
    EXPECT_NE(information.attributes & FX_DIRECTORY, 0U);
    ASSERT_EQ(runtime::filex::information("/sd/", information), 0);
    EXPECT_NE(information.attributes & FX_DIRECTORY, 0U);

    const int file{ _open("/sd/roundtrip.bin", O_CREAT | O_RDWR | O_TRUNC, 0666) };
    ASSERT_GE(file, 3);
    constexpr char payload[]{ "persistent SD media" };
    ASSERT_EQ(_write(file, payload, sizeof(payload)), static_cast<int>(sizeof(payload)));
    ASSERT_EQ(_lseek(file, 0, SEEK_SET), 0);
    std::array<char, sizeof(payload)> result{};
    ASSERT_EQ(_read(file, result.data(), result.size()), static_cast<int>(result.size()));
    EXPECT_EQ(result, std::to_array(payload));
    EXPECT_EQ(_close(file), 0);
}

TEST_F(FileXTest, FlushesFilesAndMetadataAtDurabilityBoundaries)
{
    FX_MEDIA* const flash{ runtime::storage::media(runtime::storage::Volume::flash) };
    FX_MEDIA* const sd{ runtime::storage::media(runtime::storage::Volume::sd) };

    ULONG flushes{ flash->fx_media_flushes };
    const int flash_file{ _open("/flash/durable-empty.bin", O_CREAT | O_RDONLY, 0666) };
    ASSERT_GE(flash_file, 3);
    ASSERT_EQ(_close(flash_file), 0);
    EXPECT_GT(flash->fx_media_flushes, flushes);
    EXPECT_EQ(flash->fx_media_sector_cache_dirty_count, 0U);

    flushes = sd->fx_media_flushes;
    const int sd_file{ _open("/sd/durable-empty.bin", O_CREAT | O_WRONLY | O_TRUNC, 0666) };
    ASSERT_GE(sd_file, 3);
    ASSERT_EQ(_close(sd_file), 0);
    EXPECT_GT(sd->fx_media_flushes, flushes);
    EXPECT_EQ(sd->fx_media_sector_cache_dirty_count, 0U);

    flushes = flash->fx_media_flushes;
    ASSERT_EQ(_mkdir("/flash/durable-directory", 0777), 0);
    EXPECT_GT(flash->fx_media_flushes, flushes);
    EXPECT_EQ(flash->fx_media_sector_cache_dirty_count, 0U);

    flushes = flash->fx_media_flushes;
    ASSERT_EQ(_rename("/flash/durable-directory", "/flash/durable-renamed-directory"), 0);
    EXPECT_GT(flash->fx_media_flushes, flushes);
    EXPECT_EQ(flash->fx_media_sector_cache_dirty_count, 0U);

    flushes = flash->fx_media_flushes;
    ASSERT_EQ(_unlink("/flash/durable-empty.bin"), 0);
    EXPECT_GT(flash->fx_media_flushes, flushes);
    EXPECT_EQ(flash->fx_media_sector_cache_dirty_count, 0U);

    flushes = flash->fx_media_flushes;
    ASSERT_EQ(_rmdir("/flash/durable-renamed-directory"), 0);
    EXPECT_GT(flash->fx_media_flushes, flushes);
    EXPECT_EQ(flash->fx_media_sector_cache_dirty_count, 0U);
}

TEST_F(FileXTest, TimestampSetterFlushesPendingWritesAndPreservesOffsetsOnBothVolumes)
{
    for (const auto volume : { runtime::storage::Volume::flash, runtime::storage::Volume::sd }) {
        const char* const path{ volume == runtime::storage::Volume::flash ? "/flash/timestamp.bin"
                                                                          : "/sd/timestamp.bin" };
        const int file{ _open(path, O_CREAT | O_RDWR | O_TRUNC, 0666) };
        ASSERT_GE(file, 3);
        ASSERT_EQ(_write(file, "abc", 3), 3);
        const utimbuf requested{ .actime = 0, .modtime = 1'704'164'645 }; // Odd second rounds down.
        FX_MEDIA* const media{ runtime::storage::media(volume) };
        const ULONG flushes{ media->fx_media_flushes };
        ASSERT_EQ(_utime(path, &requested), 0);
        EXPECT_GT(media->fx_media_flushes, flushes);
        EXPECT_EQ(media->fx_media_sector_cache_dirty_count, 0U);
        EXPECT_EQ(_lseek(file, 0, SEEK_CUR), 3);
        struct stat metadata{};
        ASSERT_EQ(_fstat(file, &metadata), 0);
        EXPECT_EQ(metadata.st_mtime, requested.modtime - 1);
        EXPECT_EQ(metadata.st_size, 3);
        ASSERT_EQ(_fsync(file), 0);
        ASSERT_EQ(_close(file), 0);
        ASSERT_EQ(_stat(path, &metadata), 0);
        EXPECT_EQ(metadata.st_mtime, requested.modtime - 1);
        const int reader{ _open(path, O_RDONLY) };
        ASSERT_GE(reader, 3);
        std::array<char, 3U> content{};
        EXPECT_EQ(_read(reader, content.data(), content.size()), 3);
        EXPECT_EQ(content, (std::array<char, 3U>{ 'a', 'b', 'c' }));
        EXPECT_EQ(_close(reader), 0);
    }
}

TEST_F(FileXTest, TimestampSetterSupportsDirectoriesCurrentTimeAndFatDateLimits)
{
    using namespace std::chrono;
    const auto seconds_at = [](year_month_day date) {
        return duration_cast<seconds>(sys_days{ date }.time_since_epoch()).count();
    };
    ASSERT_EQ(_mkdir("/flash/timestamp-directory", 0777), 0);
    const auto earliest{ seconds_at(year{ 1980 } / January / 1) };
    const auto after_latest{ seconds_at(year{ 2108 } / January / 1) };
    struct stat metadata{};
    for (const auto timestamp : { earliest, after_latest - 1 }) {
        const utimbuf requested{ .actime = 0, .modtime = timestamp };
        ASSERT_EQ(_utime("/flash/timestamp-directory", &requested), 0);
        ASSERT_EQ(_stat("/flash/timestamp-directory", &metadata), 0);
        EXPECT_EQ(metadata.st_mtime, timestamp & ~1LL);
    }
    for (const auto timestamp : { earliest - 1, after_latest, std::numeric_limits<time_t>::max() }) {
        const utimbuf requested{ .actime = 0, .modtime = timestamp };
        EXPECT_EQ(_utime("/flash/timestamp-directory", &requested), -1);
        EXPECT_EQ(errno, EOVERFLOW);
    }
    const auto before{ floor<seconds>(system_clock::now()).time_since_epoch().count() };
    ASSERT_EQ(_utime("/flash/timestamp-directory", nullptr), 0);
    const auto after{ floor<seconds>(system_clock::now()).time_since_epoch().count() };
    ASSERT_EQ(_stat("/flash/timestamp-directory", &metadata), 0);
    EXPECT_GE(metadata.st_mtime, before - 1);
    EXPECT_LE(metadata.st_mtime, after);
}

TEST_F(FileXTest, TimestampSetterRejectsMissingPathsAndVolumeRoots)
{
    const utimbuf requested{ .actime = 0, .modtime = 1'704'164'644 };
    for (const auto* path : { "/", "/flash", "/sd" }) {
        EXPECT_EQ(_utime(path, &requested), -1);
        EXPECT_EQ(errno, EROFS);
    }
    EXPECT_EQ(_utime("/flash/no-such-timestamp-file", &requested), -1);
    EXPECT_EQ(errno, ENOENT);
    EXPECT_EQ(_utime(nullptr, &requested), -1);
    EXPECT_EQ(errno, ENOENT);
}

TEST_F(FileXTest, RejectsCrossVolumeRenameWithoutLosingTheSource)
{
    const int file{ _open("/flash/cross-volume.bin", O_CREAT | O_WRONLY | O_TRUNC, 0666) };
    ASSERT_GE(file, 3);
    ASSERT_EQ(_close(file), 0);

    errno = 0;
    EXPECT_EQ(_rename("/flash/cross-volume.bin", "/sd/cross-volume.bin"), -1);
    EXPECT_EQ(errno, EXDEV);
    struct stat metadata{};
    EXPECT_EQ(_stat("/flash/cross-volume.bin", &metadata), 0);
    errno = 0;
    EXPECT_EQ(_stat("/sd/cross-volume.bin", &metadata), -1);
    EXPECT_EQ(errno, ENOENT);
}

TEST_F(FileXTest, RejectsPathsBeyondFileXLimit)
{
    std::string path(runtime::filex::MAXIMUM_PATH, 'x');
    char normalized[runtime::filex::MAXIMUM_PATH]{};
    EXPECT_EQ(runtime::filex::normalizePath(path.c_str(), normalized), ENAMETOOLONG);
}

TEST_F(FileXTest, NewlibDescriptorRoundTripAndMetadata)
{
    const int file{ _open("/flash/mode.bin", O_CREAT | O_RDWR | O_TRUNC, 0666) };
    ASSERT_GE(file, 3);
    constexpr char payload[]{ "FileX descriptor mode switching" };
    ASSERT_EQ(_write(file, payload, sizeof(payload)), static_cast<int>(sizeof(payload)));
    ASSERT_EQ(_lseek(file, 0, SEEK_SET), 0);
    std::array<char, sizeof(payload)> result{};
    EXPECT_EQ(_read(file, result.data(), result.size()), static_cast<int>(result.size()));
    EXPECT_EQ(result, std::to_array(payload));

    struct stat metadata{};
    EXPECT_EQ(_fstat(file, &metadata), 0);
    EXPECT_EQ(metadata.st_size, static_cast<off_t>(sizeof(payload)));
    EXPECT_EQ(_close(file), 0);
    EXPECT_EQ(_stat("/flash/mode.bin", &metadata), 0);
    EXPECT_TRUE(S_ISREG(metadata.st_mode));
}

TEST_F(FileXTest, ZeroLengthFileIoAcceptsNullBuffersForValidDescriptors)
{
    const int file{ _open("/flash/mode.bin", O_CREAT | O_RDWR | O_TRUNC, 0666) };
    ASSERT_GE(file, 3);
    EXPECT_EQ(_read(file, nullptr, 0), 0);
    EXPECT_EQ(_write(file, nullptr, 0), 0);

    errno = 0;
    EXPECT_EQ(_read(99, nullptr, 0), -1);
    EXPECT_EQ(errno, EBADF);
    errno = 0;
    EXPECT_EQ(_write(99, nullptr, 0), -1);
    EXPECT_EQ(errno, EBADF);
    EXPECT_EQ(_close(file), 0);
}

TEST_F(FileXTest, ConsoleIoValidatesBuffersAndReportsPartialTransfers)
{
    console_input_calls = 0;
    console_input_failure_after = -1;
    console_input_newline_at = -1;
    console_output_calls = 0;
    console_output_failure_after = -1;

    EXPECT_EQ(_read(STDIN_FILENO, nullptr, 0), 0);
    EXPECT_EQ(_write(STDOUT_FILENO, nullptr, 0), 0);

    errno = 0;
    EXPECT_EQ(_read(STDIN_FILENO, nullptr, 1), -1);
    EXPECT_EQ(errno, EFAULT);
    errno = 0;
    EXPECT_EQ(_write(STDERR_FILENO, nullptr, 1), -1);
    EXPECT_EQ(errno, EFAULT);

    char input[4]{};
    console_input_calls = 0;
    console_input_failure_after = 2;
    EXPECT_EQ(_read(STDIN_FILENO, input, sizeof(input)), 2);
    EXPECT_EQ(std::string_view(input, 2), "xx");
    console_input_calls = 0;
    console_input_failure_after = 0;
    errno = 0;
    EXPECT_EQ(_read(STDIN_FILENO, input, sizeof(input)), -1);
    EXPECT_EQ(errno, EIO);

    console_input_calls = 0;
    console_input_failure_after = -1;
    console_input_newline_at = 2;
    EXPECT_EQ(_read(STDIN_FILENO, input, sizeof(input)), 3);
    EXPECT_EQ(std::string_view(input, 3), "xx\n");

    constexpr char output[]{ "abcd" };
    console_output_calls = 0;
    console_output_failure_after = 2;
    EXPECT_EQ(_write(STDOUT_FILENO, output, 4), 2);
    console_output_calls = 0;
    console_output_failure_after = 0;
    errno = 0;
    EXPECT_EQ(_write(STDOUT_FILENO, output, 4), -1);
    EXPECT_EQ(errno, EIO);

    console_input_failure_after = -1;
    console_input_newline_at = -1;
    console_output_failure_after = -1;
}

TEST_F(FileXTest, SeekRejectsOffsetsThatCannotBeRepresentedWithoutOverflow)
{
    const int file{ _open("/flash/seek-overflow.bin", O_CREAT | O_RDWR | O_TRUNC, 0666) };
    ASSERT_GE(file, 3);
    ASSERT_EQ(_lseek(file, std::numeric_limits<off_t>::max(), SEEK_SET), std::numeric_limits<off_t>::max());

    errno = 0;
    EXPECT_EQ(_lseek(file, 1, SEEK_CUR), static_cast<off_t>(-1));
    EXPECT_EQ(errno, EOVERFLOW);
    EXPECT_EQ(_close(file), 0);
}

TEST_F(FileXTest, RenameReplacesDestinationAndPreservesOpenSourceDescriptor)
{
    int destination{ _open("/flash/rename-destination.bin", O_CREAT | O_WRONLY | O_TRUNC, 0666) };
    ASSERT_GE(destination, 3);
    ASSERT_EQ(_write(destination, "old", 3), 3);
    ASSERT_EQ(_close(destination), 0);

    const int source{ _open("/flash/rename-source.bin", O_CREAT | O_RDWR | O_TRUNC, 0666) };
    ASSERT_GE(source, 3);
    constexpr char payload[]{ "replacement" };
    ASSERT_EQ(_write(source, payload, sizeof(payload)), static_cast<int>(sizeof(payload)));
    ASSERT_EQ(_rename("/flash/rename-source.bin", "/flash/rename-destination.bin"), 0);

    struct stat metadata{};
    EXPECT_EQ(_fstat(source, &metadata), 0);
    EXPECT_EQ(metadata.st_size, static_cast<off_t>(sizeof(payload)));
    ASSERT_EQ(_lseek(source, 0, SEEK_SET), 0);
    std::array<char, sizeof(payload)> result{};
    errno = 0;
    const int read_result{ _read(source, result.data(), result.size()) };
    EXPECT_EQ(read_result, static_cast<int>(result.size())) << std::strerror(errno);
    EXPECT_EQ(result, std::to_array(payload));
    EXPECT_EQ(_close(source), 0);

    errno = 0;
    EXPECT_EQ(_stat("/flash/rename-source.bin", &metadata), -1);
    EXPECT_EQ(errno, ENOENT);
    EXPECT_EQ(_stat("/flash/rename-destination.bin", &metadata), 0);
}

TEST_F(FileXTest, RenameDoesNotRetargetAnOpenDestinationDescriptor)
{
    const int source{ _open("/flash/open-rename-source.bin", O_CREAT | O_WRONLY | O_TRUNC, 0666) };
    ASSERT_GE(source, 3);
    ASSERT_EQ(_write(source, "source", 6), 6);
    ASSERT_EQ(_close(source), 0);

    const int destination{ _open("/flash/open-rename-destination.bin", O_CREAT | O_RDWR | O_TRUNC, 0666) };
    ASSERT_GE(destination, 3);
    ASSERT_EQ(_write(destination, "destination", 11), 11);

    errno = 0;
    EXPECT_EQ(_rename("/flash/open-rename-source.bin", "/flash/open-rename-destination.bin"), -1);
    EXPECT_EQ(errno, EBUSY);

    ASSERT_EQ(_lseek(destination, 0, SEEK_SET), 0);
    std::array<char, 11> destination_contents{};
    EXPECT_EQ(_read(destination, destination_contents.data(), destination_contents.size()),
              static_cast<int>(destination_contents.size()));
    EXPECT_EQ(std::string_view(destination_contents.data(), destination_contents.size()), "destination");
    EXPECT_EQ(_close(destination), 0);

    struct stat metadata{};
    EXPECT_EQ(_stat("/flash/open-rename-source.bin", &metadata), 0);
    EXPECT_EQ(_stat("/flash/open-rename-destination.bin", &metadata), 0);
}

TEST_F(FileXTest, RenamePreservesANonemptyDestinationDirectoryOnFailure)
{
    ASSERT_EQ(_mkdir("/flash/rename-source-directory", 0777), 0);
    ASSERT_EQ(_mkdir("/flash/rename-destination-directory", 0777), 0);
    const int child{ _open(
      "/flash/rename-destination-directory/child.bin", O_CREAT | O_WRONLY | O_TRUNC, 0666) };
    ASSERT_GE(child, 3);
    ASSERT_EQ(_close(child), 0);

    errno = 0;
    EXPECT_EQ(_rename("/flash/rename-source-directory", "/flash/rename-destination-directory"), -1);
    EXPECT_EQ(errno, ENOTEMPTY);

    struct stat metadata{};
    EXPECT_EQ(_stat("/flash/rename-source-directory", &metadata), 0);
    EXPECT_TRUE(S_ISDIR(metadata.st_mode));
    EXPECT_EQ(_stat("/flash/rename-destination-directory", &metadata), 0);
    EXPECT_TRUE(S_ISDIR(metadata.st_mode));
    EXPECT_EQ(_stat("/flash/rename-destination-directory/child.bin", &metadata), 0);

    ASSERT_EQ(_unlink("/flash/rename-destination-directory/child.bin"), 0);
    EXPECT_EQ(_rename("/flash/rename-source-directory", "/flash/rename-destination-directory"), 0);
    errno = 0;
    EXPECT_EQ(_stat("/flash/rename-source-directory", &metadata), -1);
    EXPECT_EQ(errno, ENOENT);
    EXPECT_EQ(_stat("/flash/rename-destination-directory", &metadata), 0);
    EXPECT_TRUE(S_ISDIR(metadata.st_mode));
}

TEST_F(FileXTest, RenameMigratesCurrentDirectoryAndRelativePaths)
{
    ASSERT_EQ(_mkdir("/flash/Original-Cwd", 0777), 0);
    ASSERT_EQ(_mkdir("/flash/Original-Cwd/child", 0777), 0);
    ASSERT_EQ(runtime::filex::setCurrentPath("/flash/original-cwd/child"), 0);
    ASSERT_EQ(_rename("/flash/ORIGINAL-CWD", "/flash/RENAMED-CWD"), 0);

    char current[runtime::filex::MAXIMUM_PATH]{};
    EXPECT_EQ(runtime::filex::currentPath(current), 0);
    EXPECT_STREQ(current, "/flash/RENAMED-CWD/child");

    const int file{ _open("relative.bin", O_CREAT | O_WRONLY | O_TRUNC, 0666) };
    ASSERT_GE(file, 3);
    EXPECT_EQ(_close(file), 0);
    struct stat metadata{};
    EXPECT_EQ(_stat("/flash/renamed-cwd/child/relative.bin", &metadata), 0);
}

TEST_F(FileXTest, CaseInsensitiveAliasesPreserveIdentityAndOpenDescriptors)
{
    const int file{ _open("/flash/Case-Name.bin", O_CREAT | O_RDWR | O_TRUNC, 0666) };
    ASSERT_GE(file, 3);
    constexpr char payload[]{ "case-insensitive FileX identity" };
    ASSERT_EQ(_write(file, payload, sizeof(payload)), static_cast<int>(sizeof(payload)));

    ASSERT_EQ(_rename("/flash/case-name.bin", "/flash/CASE-NAME.BIN"), 0);
    struct stat upper_metadata{};
    struct stat lower_metadata{};
    ASSERT_EQ(_stat("/flash/CASE-NAME.BIN", &upper_metadata), 0);
    ASSERT_EQ(_stat("/flash/case-name.bin", &lower_metadata), 0);
    EXPECT_EQ(upper_metadata.st_ino, lower_metadata.st_ino);

    ASSERT_EQ(_rename("/flash/case-name.bin", "/flash/Mixed-Target.bin"), 0);
    ASSERT_EQ(_lseek(file, 0, SEEK_SET), 0);
    std::array<char, sizeof(payload)> result{};
    EXPECT_EQ(_read(file, result.data(), result.size()), static_cast<int>(result.size()));
    EXPECT_EQ(result, std::to_array(payload));
    EXPECT_EQ(_close(file), 0);

    struct stat metadata{};
    errno = 0;
    EXPECT_EQ(_stat("/flash/case-name.bin", &metadata), -1);
    EXPECT_EQ(errno, ENOENT);
    EXPECT_EQ(_stat("/flash/mixed-target.bin", &metadata), 0);
}

TEST_F(FileXTest, RejectsDirectoryMovesBelowItselfAndRemovalOfCurrentDirectory)
{
    ASSERT_EQ(_mkdir("/flash/self-tree", 0777), 0);
    ASSERT_EQ(_mkdir("/flash/self-tree/child", 0777), 0);
    errno = 0;
    EXPECT_EQ(_rename("/flash/self-tree", "/flash/self-tree/child/grandchild"), -1);
    EXPECT_EQ(errno, EINVAL);

    struct stat metadata{};
    EXPECT_EQ(_stat("/flash/self-tree", &metadata), 0);
    EXPECT_EQ(_stat("/flash/self-tree/child", &metadata), 0);

    ASSERT_EQ(_mkdir("/flash/working-directory", 0777), 0);
    ASSERT_EQ(runtime::filex::setCurrentPath("/flash/working-directory"), 0);
    errno = 0;
    EXPECT_EQ(_rmdir("/flash/WORKING-DIRECTORY"), -1);
    EXPECT_EQ(errno, EBUSY);
    ASSERT_EQ(runtime::filex::setCurrentPath("/flash/"), 0);
    EXPECT_EQ(_rmdir("/flash/working-directory"), 0);
}

TEST_F(FileXTest, DeletesDirectoryWithManyFormerEntriesOnSmallFatVolume)
{
    ASSERT_EQ(_mkdir("/flash/entry-history", 0777), 0);
    for (unsigned int index{}; index < 16U; ++index) {
        const std::string path{ "/flash/entry-history/" + std::to_string(index) + ".txt" };
        const int file{ _open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0666) };
        ASSERT_GE(file, 3);
        ASSERT_EQ(_close(file), 0);
        ASSERT_EQ(_unlink(path.c_str()), 0);
    }
    EXPECT_EQ(_rmdir("/flash/entry-history"), 0);
}

TEST_F(FileXTest, AppendAndTruncateAreDeterministic)
{
    int file{ _open("/flash/append.bin", O_CREAT | O_WRONLY | O_TRUNC, 0666) };
    ASSERT_GE(file, 3);
    ASSERT_EQ(_write(file, "one", 3), 3);
    ASSERT_EQ(_close(file), 0);

    file = _open("/flash/append.bin", O_WRONLY | O_APPEND);
    ASSERT_GE(file, 3);
    ASSERT_EQ(_lseek(file, 0, SEEK_SET), 0);
    ASSERT_EQ(_write(file, "two", 3), 3);
    ASSERT_EQ(_ftruncate(file, 4), 0);
    ASSERT_EQ(_close(file), 0);

    file = _open("/flash/append.bin", O_RDONLY);
    ASSERT_GE(file, 3);
    std::array<char, 8> result{};
    EXPECT_EQ(_read(file, result.data(), result.size()), 4);
    EXPECT_EQ(std::string_view(result.data(), 4), "onet");
    EXPECT_EQ(_close(file), 0);
}

TEST_F(FileXTest, ExtendingAFileZeroFillsTheNewRange)
{
    int file{ _open("/flash/truncate.bin", O_CREAT | O_RDWR | O_TRUNC, 0666) };
    ASSERT_GE(file, 3);
    ASSERT_EQ(_write(file, "abc", 3), 3);
    ASSERT_EQ(_ftruncate(file, 8), 0);
    ASSERT_EQ(_lseek(file, 0, SEEK_SET), 0);
    std::array<unsigned char, 8> result{};
    ASSERT_EQ(_read(file, reinterpret_cast<char*>(result.data()), result.size()),
              static_cast<int>(result.size()));
    EXPECT_EQ(result[0], 'a');
    EXPECT_EQ(result[1], 'b');
    EXPECT_EQ(result[2], 'c');
    EXPECT_EQ(result[3], 0U);
    EXPECT_EQ(result[7], 0U);
    EXPECT_EQ(_close(file), 0);
}

TEST_F(FileXTest, SeekingBeyondEofPreservesOffsetAndZeroFillsOnWrite)
{
    const int file{ _open("/flash/seek-gap.bin", O_CREAT | O_RDWR | O_TRUNC, 0666) };
    ASSERT_GE(file, 3);
    ASSERT_EQ(_write(file, "abc", 3), 3);
    ASSERT_EQ(_lseek(file, 8, SEEK_SET), 8);

    struct stat metadata{};
    ASSERT_EQ(_fstat(file, &metadata), 0);
    EXPECT_EQ(metadata.st_size, 3);

    // This switches FileX's handle to read mode, where its physical seek
    // position is clamped to EOF, then switches it back for the write.
    char unused{};
    EXPECT_EQ(_read(file, &unused, 1), 0);
    EXPECT_EQ(_lseek(file, 0, SEEK_CUR), 8);
    ASSERT_EQ(_write(file, "z", 1), 1);
    EXPECT_EQ(_lseek(file, 0, SEEK_CUR), 9);
    ASSERT_EQ(_lseek(file, 0, SEEK_SET), 0);
    std::array<char, 9U> result{};
    ASSERT_EQ(_read(file, result.data(), result.size()), static_cast<int>(result.size()));
    const std::array<char, 9U> expected{ 'a', 'b', 'c', 0, 0, 0, 0, 0, 'z' };
    EXPECT_EQ(result, expected);
    EXPECT_EQ(_close(file), 0);
}

TEST_F(FileXTest, WriteOnlySeekBeyondEofZeroFillsTheGap)
{
    int file{ _open("/flash/seek-gap.bin", O_CREAT | O_WRONLY | O_TRUNC, 0666) };
    ASSERT_GE(file, 3);
    ASSERT_EQ(_lseek(file, 3, SEEK_SET), 3);
    ASSERT_EQ(_write(file, "x", 1), 1);
    ASSERT_EQ(_close(file), 0);
    file = _open("/flash/seek-gap.bin", O_RDONLY);
    ASSERT_GE(file, 3);
    std::array<char, 4U> result{};
    EXPECT_EQ(_read(file, result.data(), result.size()), static_cast<int>(result.size()));
    const std::array<char, 4U> expected{ 0, 0, 0, 'x' };
    EXPECT_EQ(result, expected);
    EXPECT_EQ(_close(file), 0);
}

TEST_F(FileXTest, TruncatePreservesOffsetAndReportsNewSizeBeforeClose)
{
    const int file{ _open("/flash/truncate.bin", O_CREAT | O_RDWR | O_TRUNC, 0666) };
    ASSERT_GE(file, 3);
    ASSERT_EQ(_write(file, "abcdef", 6), 6);
    ASSERT_EQ(_fsync(file), 0);
    ASSERT_EQ(_ftruncate(file, 2), 0);
    EXPECT_EQ(_lseek(file, 0, SEEK_CUR), 6);
    struct stat metadata{};
    ASSERT_EQ(_stat("/flash/truncate.bin", &metadata), 0);
    EXPECT_EQ(metadata.st_size, 2);
    ASSERT_EQ(_write(file, "z", 1), 1);
    ASSERT_EQ(_lseek(file, 0, SEEK_SET), 0);
    std::array<char, 7U> result{};
    ASSERT_EQ(_read(file, result.data(), result.size()), static_cast<int>(result.size()));
    const std::array<char, 7U> expected{ 'a', 'b', 0, 0, 0, 0, 'z' };
    EXPECT_EQ(result, expected);
    ASSERT_EQ(_lseek(file, 12, SEEK_SET), 12);
    ASSERT_EQ(_ftruncate(file, 9), 0);
    EXPECT_EQ(_lseek(file, 0, SEEK_CUR), 12);
    EXPECT_EQ(_close(file), 0);
}

TEST_F(FileXTest, InvalidAccessModeDoesNotCreateAFile)
{
    errno = 0;
    EXPECT_EQ(_open("/flash/invalid-flags.bin", O_CREAT | O_ACCMODE, 0666), -1);
    EXPECT_EQ(errno, EINVAL);
    struct stat metadata{};
    EXPECT_EQ(_stat("/flash/invalid-flags.bin", &metadata), -1);
    EXPECT_EQ(errno, ENOENT);
}

TEST_F(FileXTest, NewReaderSeesUnflushedWritesWithALowerDescriptorThanTheWriter)
{
    // Reserve the first slot, then release it so a newly opened reader is
    // visited before the writer when the adapter searches its registry.
    const int placeholder{ _open("/flash/mode.bin", O_CREAT | O_RDONLY, 0666) };
    ASSERT_GE(placeholder, 3);
    const int writer{ _open("/flash/shared.bin", O_CREAT | O_WRONLY | O_TRUNC, 0666) };
    ASSERT_GT(writer, placeholder);
    ASSERT_EQ(_close(placeholder), 0);
    ASSERT_EQ(_write(writer, "abc", 3), 3);
    const int reader{ _open("/flash/shared.bin", O_RDONLY) };
    ASSERT_EQ(reader, placeholder);

    struct stat metadata{};
    ASSERT_EQ(_stat("/flash/shared.bin", &metadata), 0);
    EXPECT_EQ(metadata.st_size, 3);
    ASSERT_EQ(_fstat(reader, &metadata), 0);
    EXPECT_EQ(metadata.st_size, 3);
    std::array<char, 3U> result{};
    EXPECT_EQ(_read(reader, result.data(), result.size()), static_cast<int>(result.size()));
    EXPECT_EQ(std::string_view(result.data(), result.size()), "abc");

    ASSERT_EQ(_ftruncate(writer, 1), 0);
    ASSERT_EQ(_stat("/flash/shared.bin", &metadata), 0);
    EXPECT_EQ(metadata.st_size, 1);
    ASSERT_EQ(_fstat(reader, &metadata), 0);
    EXPECT_EQ(metadata.st_size, 1);
    EXPECT_EQ(_close(reader), 0);
    EXPECT_EQ(_close(writer), 0);
}

TEST_F(FileXTest, RejectsExtremeInvalidDescriptorsWithoutOverflow)
{
    errno = 0;
    EXPECT_EQ(_close(std::numeric_limits<int>::min()), -1);
    EXPECT_EQ(errno, EBADF);
    EXPECT_EQ(_lseek(std::numeric_limits<int>::min(), 0, SEEK_SET), -1);
    EXPECT_EQ(errno, EBADF);
}

TEST_F(FileXTest, DescriptorTableHasExactlySixteenFileSlots)
{
    std::vector<int> files;
    for (std::size_t index{}; index < runtime::filex::MAXIMUM_OPEN_FILES; ++index) {
        const std::string path{ "/flash/slot" + std::to_string(index) };
        const int file{ _open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0666) };
        ASSERT_GE(file, 3);
        files.push_back(file);
    }
    errno = 0;
    EXPECT_EQ(_open("/flash/overflow", O_CREAT | O_RDWR, 0666), -1);
    EXPECT_EQ(errno, EMFILE);
    for (int file : files) {
        EXPECT_EQ(_close(file), 0);
    }
    for (std::size_t index{}; index < files.size(); ++index) {
        const std::string path{ "/flash/slot" + std::to_string(index) };
        EXPECT_EQ(_unlink(path.c_str()), 0);
    }
    errno = 0;
    EXPECT_EQ(_unlink("/flash/overflow"), -1);
    EXPECT_EQ(errno, ENOENT);
}

TEST_F(FileXTest, RenameDirectoriesAndInvalidHandles)
{
    EXPECT_EQ(_mkdir("/flash/directory", 0777), 0);
    errno = 0;
    EXPECT_EQ(_unlink("/flash/directory"), -1);
    EXPECT_EQ(errno, EISDIR);
    EXPECT_EQ(_rmdir("/flash/directory"), 0);
    int file{ _open("/flash/mode.bin", O_CREAT | O_WRONLY | O_TRUNC, 0666) };
    ASSERT_GE(file, 3);
    EXPECT_EQ(_close(file), 0);
    errno = 0;
    EXPECT_EQ(_rmdir("/flash/mode.bin"), -1);
    EXPECT_EQ(errno, ENOTDIR);
    EXPECT_EQ(_rename("/flash/mode.bin", "/flash/renamed.bin"), 0);
    errno = 0;
    char unused{};
    EXPECT_EQ(_read(file, &unused, 0), -1);
    EXPECT_EQ(errno, EBADF);
}

TEST_F(FileXTest, SerializesSharedAndDistinctThreadAccess)
{
    const int shared{ _open("/flash/shared.bin", O_CREAT | O_WRONLY | O_TRUNC, 0666) };
    ASSERT_GE(shared, 3);
    constexpr std::array<char, 64> payload{};
    std::array<std::thread, 4> threads;
    for (unsigned index{}; index < threads.size(); ++index) {
        threads[index] = std::thread{ [shared, index, &payload] {
            EXPECT_EQ(_write(shared, payload.data(), payload.size()), static_cast<int>(payload.size()));
            const std::string path{ "/flash/concurrent" + std::to_string(index) };
            const int file{ _open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0666) };
            ASSERT_GE(file, 3);
            EXPECT_EQ(_write(file, payload.data(), payload.size()), static_cast<int>(payload.size()));
            EXPECT_EQ(_close(file), 0);
        } };
    }
    for (auto& thread : threads) {
        thread.join();
    }
    struct stat metadata{};
    EXPECT_EQ(_fstat(shared, &metadata), 0);
    EXPECT_EQ(metadata.st_size, static_cast<off_t>(payload.size() * threads.size()));
    EXPECT_EQ(_close(shared), 0);
}
