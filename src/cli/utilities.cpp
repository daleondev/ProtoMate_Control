#include "utilities.hpp"

#include "Parser.hpp"
#include "filesystem.hpp"

#include "pneumo/meta.hpp"

#include <malloc.h>
#include <tx_thread.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <istream>
#include <iterator>
#include <mutex>
#include <optional>
#include <ostream>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(PNM_PLATFORM_LINUX)
#include "tx_thread_stack_info.hpp"
#endif

#if !defined(PNM_PLATFORM_LINUX)
extern "C" {
extern std::byte __HeapBase;
extern std::byte __HeapLimit;
}
#endif

namespace cli::utilities
{
    namespace commands
    {
        namespace
        {
            using namespace pnm::meta::string::literals;
            namespace fs = std::filesystem;

            [[nodiscard]] auto path_text(const fs::path& path) -> std::string
            {
                const auto text{ path.generic_string() };
                return text.empty() ? std::string{ "." } : text;
            }

            [[nodiscard]] auto stream_error(std::string_view action, const fs::path& path) -> CallbackResult
            {
                const std::error_code error{ errno == 0 ? EIO : errno, std::generic_category() };
                return callback_failure(std::string{ action } + " '" + path_text(path) +
                                        "': " + error.message());
            }

            [[nodiscard]] auto contains(std::string_view line, std::string_view pattern, bool ignore_case)
              -> bool
            {
                if (!ignore_case) {
                    return line.find(pattern) != std::string_view::npos;
                }

                const auto same_character = [](char left, char right) {
                    return std::tolower(static_cast<unsigned char>(left)) ==
                           std::tolower(static_cast<unsigned char>(right));
                };
                return std::ranges::search(line, pattern, same_character).begin() != line.end();
            }

            struct ThreadInfo
            {
                std::uint32_t prio;
                std::string_view name;
                std::size_t stack_size;
                std::size_t current_stack_usage;
                std::size_t peak_stack_usage;
            };

            struct HeapInfo
            {
                std::string_view allocator;
                std::optional<std::size_t> capacity;
                std::size_t reserved;
                std::size_t used;
                std::size_t available;
                std::size_t releasable;
                std::size_t free_blocks;
                std::size_t mapped_regions;
                std::size_t mapped_bytes;
            };

            auto refreshThreadStackInfo([[maybe_unused]] TX_THREAD* thread) -> void
            {
#if defined(TX_ENABLE_STACK_CHECKING) && !defined(PNM_PLATFORM_LINUX)
                if (thread != TX_NULL && thread != _tx_thread_current_ptr &&
                    thread->tx_thread_stack_ptr != TX_NULL &&
                    thread->tx_thread_stack_highest_ptr != TX_NULL &&
                    reinterpret_cast<uintptr_t>(thread->tx_thread_stack_ptr) <
                      reinterpret_cast<uintptr_t>(thread->tx_thread_stack_highest_ptr)) {
                    thread->tx_thread_stack_highest_ptr = thread->tx_thread_stack_ptr;
                }
#endif
            }

            auto getThreadInformation(TX_THREAD* thread) -> ThreadInfo
            {
                refreshThreadStackInfo(thread);

                const auto stack_end = reinterpret_cast<uintptr_t>(thread->tx_thread_stack_end);
                const auto current_stack_ptr = reinterpret_cast<uintptr_t>(thread->tx_thread_stack_ptr);
                const auto peak_stack_ptr = reinterpret_cast<uintptr_t>(thread->tx_thread_stack_highest_ptr);
                PNM_ASSERT(current_stack_ptr <= stack_end && peak_stack_ptr <= stack_end, "Invalid stack");

                return { .prio = thread->tx_thread_priority,
                         .name =
                           thread->tx_thread_name == TX_NULL ? std::string_view{} : thread->tx_thread_name,
                         .stack_size = static_cast<std::size_t>(thread->tx_thread_stack_size),
                         .current_stack_usage = static_cast<std::size_t>(stack_end - current_stack_ptr),
                         .peak_stack_usage = static_cast<std::size_t>(stack_end - peak_stack_ptr) };
            }

            auto getAllThreadsInformation() -> std::vector<ThreadInfo>
            {
                auto* first_thread{ _tx_thread_created_ptr };
                if (first_thread == TX_NULL || _tx_thread_created_count == 0U) {
                    return {};
                }

                std::vector<ThreadInfo> threads_info{};
                threads_info.reserve(_tx_thread_created_count);

                auto* current_thread{ first_thread };
                do {
                    threads_info.push_back(getThreadInformation(current_thread));
                    current_thread = current_thread->tx_thread_created_next;
                } while (current_thread != first_thread);

                return threads_info;
            }

            [[nodiscard]] auto getHeapInformation() -> HeapInfo
            {
#if defined(PNM_PLATFORM_LINUX)
                const struct mallinfo2 allocator_info{ ::mallinfo2() };
                return {
                    .allocator = "glibc",
                    .capacity = std::nullopt,
                    .reserved = allocator_info.arena + allocator_info.hblkhd,
                    .used = allocator_info.uordblks + allocator_info.hblkhd,
                    .available = allocator_info.fordblks,
                    .releasable = allocator_info.keepcost,
                    .free_blocks = allocator_info.ordblks,
                    .mapped_regions = allocator_info.hblks,
                    .mapped_bytes = allocator_info.hblkhd,
                };
#else
                const struct mallinfo allocator_info{ ::mallinfo() };
                const auto heap_base{ reinterpret_cast<std::uintptr_t>(&__HeapBase) };
                const auto heap_limit{ reinterpret_cast<std::uintptr_t>(&__HeapLimit) };
                PNM_ASSERT(heap_base <= heap_limit, "Invalid heap bounds");

                const std::size_t capacity{ heap_limit - heap_base };
                const std::size_t reserved{ allocator_info.arena };
                const std::size_t headroom{ reserved < capacity ? capacity - reserved : 0U };
                return {
                    .allocator = "Newlib",
                    .capacity = capacity,
                    .reserved = reserved,
                    .used = allocator_info.uordblks,
                    .available = allocator_info.fordblks + headroom,
                    .releasable = allocator_info.keepcost,
                    .free_blocks = allocator_info.ordblks,
                    .mapped_regions = allocator_info.hblks,
                    .mapped_bytes = allocator_info.hblkhd,
                };
#endif
            }

            using namespace std::literals;

            constexpr std::size_t BAR_WIDTH{ 30U };

            constexpr std::string_view HORIZONTAL{ "\u2500"sv };
            constexpr std::string_view TOP_LEFT{ "\u250c"sv };
            constexpr std::string_view TOP_JUNCTION{ "\u252c"sv };
            constexpr std::string_view TOP_RIGHT{ "\u2510"sv };
            constexpr std::string_view MID_LEFT{ "\u251c"sv };
            constexpr std::string_view MID_JUNCTION{ "\u253c"sv };
            constexpr std::string_view MID_RIGHT{ "\u2524"sv };
            constexpr std::string_view BOTTOM_LEFT{ "\u2514"sv };
            constexpr std::string_view BOTTOM_JUNCTION{ "\u2534"sv };
            constexpr std::string_view BOTTOM_RIGHT{ "\u2518"sv };
            constexpr std::string_view VERTICAL{ "\u2502"sv };
            constexpr std::string_view BAR_USED{ "\u2588"sv };
            constexpr std::string_view BAR_FREE{ "\u2591"sv };

            constexpr std::string_view PRIO_HEADER{ "Prio"sv };
            constexpr std::string_view NAME_HEADER{ "Name"sv };
            constexpr std::string_view AVAIL_HEADER{ "Available [bytes]"sv };
            constexpr std::string_view CURR_HEADER{ "Current [bytes]"sv };
            constexpr std::string_view PEAK_HEADER{ "Peak [bytes]"sv };
            constexpr std::string_view USAGE_HEADER{ "Usage [%]"sv };
            constexpr std::string_view BAR_HEADER{ "Stack-Usage Bar"sv };

            constexpr std::size_t HEAP_METRIC_WIDTH{ 18U };
            constexpr std::size_t HEAP_BAR_WIDTH{ 14U };
            constexpr std::size_t HEAP_METRIC_ROW_COUNT{ 10U };
            constexpr std::size_t HEAP_BAR_HEIGHT{ (HEAP_METRIC_ROW_COUNT * 2U) - 1U };
            constexpr std::string_view HEAP_METRIC_HEADER{ "Metric"sv };
            constexpr std::string_view HEAP_VALUE_HEADER{ "Value"sv };
            constexpr std::string_view HEAP_ALLOCATOR{ "Allocator"sv };
            constexpr std::string_view HEAP_CAPACITY{ "Capacity [bytes]"sv };
            constexpr std::string_view HEAP_RESERVED{ "Reserved [bytes]"sv };
            constexpr std::string_view HEAP_USED{ "Used [bytes]"sv };
            constexpr std::string_view HEAP_AVAILABLE{ "Available [bytes]"sv };
            constexpr std::string_view HEAP_RELEASABLE{ "Releasable [bytes]"sv };
            constexpr std::string_view HEAP_FREE_BLOCKS{ "Free blocks"sv };
            constexpr std::string_view HEAP_MAPPED_REGIONS{ "Mapped regions"sv };
            constexpr std::string_view HEAP_MAPPED_BYTES{ "Mapped [bytes]"sv };
            constexpr std::string_view HEAP_USAGE{ "Usage [%]"sv };
            constexpr std::string_view HEAP_BAR{ "Heap-Usage Bar"sv };

            auto writeRepeated(std::ostream& output, std::string_view value, std::size_t count) -> void
            {
                for (std::size_t index{}; index < count; ++index) {
                    output << value;
                }
            }

            auto writeBorder(std::ostream& output,
                             std::string_view left,
                             std::string_view junction,
                             std::string_view right,
                             std::span<const std::size_t> widths) -> void
            {
                output << left;
                for (std::size_t index{}; index < widths.size(); ++index) {
                    writeRepeated(output, HORIZONTAL, widths[index] + 2U);
                    output << (index + 1U == widths.size() ? right : junction);
                }
                output.put('\n');
            }

            auto writeRow(std::ostream& output,
                          const auto& priority,
                          std::string_view name,
                          std::size_t name_width,
                          const auto& available,
                          const auto& current,
                          const auto& peak,
                          const auto& usage,
                          std::string_view bar) -> void
            {
                std::format_to(std::ostreambuf_iterator<char>{ output },
                               "\u2502 {:>{}} \u2502 {:<{}} \u2502 {:>{}} \u2502 {:>{}} "
                               "\u2502 {:>{}} \u2502 {:>{}} \u2502 {:<{}} \u2502",
                               priority,
                               PRIO_HEADER.size(),
                               name,
                               name_width,
                               available,
                               AVAIL_HEADER.size(),
                               current,
                               CURR_HEADER.size(),
                               peak,
                               PEAK_HEADER.size(),
                               usage,
                               USAGE_HEADER.size(),
                               bar,
                               BAR_WIDTH);
            }

            auto writeThreadRow(std::ostream& output, const ThreadInfo& thread, std::size_t name_width)
              -> void
            {
                const std::size_t usage_percent{ (thread.peak_stack_usage * 100U) / thread.stack_size };
                const std::size_t bar_length{ std::min((usage_percent * BAR_WIDTH) / 100U, BAR_WIDTH) };

                std::format_to(std::ostreambuf_iterator<char>{ output },
                               "\u2502 {:>{}} \u2502 {:<{}} \u2502 {:>{}} \u2502 {:>{}} "
                               "\u2502 {:>{}} \u2502 {:>{}} \u2502 ",
                               thread.prio,
                               PRIO_HEADER.size(),
                               thread.name,
                               name_width,
                               thread.stack_size,
                               AVAIL_HEADER.size(),
                               thread.current_stack_usage,
                               CURR_HEADER.size(),
                               thread.peak_stack_usage,
                               PEAK_HEADER.size(),
                               usage_percent,
                               USAGE_HEADER.size());
                writeRepeated(output, BAR_USED, bar_length);
                writeRepeated(output, BAR_FREE, BAR_WIDTH - bar_length);
                output << " \u2502";
            }

            [[nodiscard]] auto decimalWidth(std::size_t value) -> std::size_t
            {
                std::size_t width{ 1U };
                while (value >= 10U) {
                    value /= 10U;
                    ++width;
                }
                return width;
            }

            auto writeHeapHeader(std::ostream& output, std::size_t value_width) -> void
            {
                std::format_to(std::ostreambuf_iterator<char>{ output },
                               "\u2502 {:<{}} \u2502 {:>{}} \u2502 {:<{}} \u2502",
                               HEAP_METRIC_HEADER,
                               HEAP_METRIC_WIDTH,
                               HEAP_VALUE_HEADER,
                               value_width,
                               HEAP_BAR,
                               HEAP_BAR_WIDTH);
                output.put('\n');
            }

            [[nodiscard]] auto heapBarUsed(std::size_t usage_percent, std::size_t row) -> bool
            {
                const std::size_t clamped_usage{ std::min(usage_percent, std::size_t{ 100U }) };
                const std::size_t used_rows{ (clamped_usage * HEAP_BAR_HEIGHT + 50U) / 100U };
                return row >= HEAP_BAR_HEIGHT - used_rows;
            }

            auto writeHeapBarCell(std::ostream& output, std::size_t usage_percent, std::size_t row) -> void
            {
                output.put(' ');
                writeRepeated(output, heapBarUsed(usage_percent, row) ? BAR_USED : BAR_FREE, HEAP_BAR_WIDTH);
                output << " \u2502\n";
            }

            auto writeHeapRowSeparator(std::ostream& output,
                                       std::size_t value_width,
                                       std::size_t usage_percent,
                                       std::size_t row) -> void
            {
                output << MID_LEFT;
                writeRepeated(output, HORIZONTAL, HEAP_METRIC_WIDTH + 2U);
                output << MID_JUNCTION;
                writeRepeated(output, HORIZONTAL, value_width + 2U);
                output << MID_RIGHT;
                writeHeapBarCell(output, usage_percent, row);
            }

            auto writeHeapRow(std::ostream& output,
                              std::string_view metric,
                              const auto& value,
                              std::size_t value_width,
                              std::size_t usage_percent,
                              std::size_t row) -> void
            {
                std::format_to(std::ostreambuf_iterator<char>{ output },
                               "\u2502 {:<{}} \u2502 {:>{}} \u2502",
                               metric,
                               HEAP_METRIC_WIDTH,
                               value,
                               value_width);
                writeHeapBarCell(output, usage_percent, row);
            }
        }

        [[
            = "Print text to standard output."_fs,
            = Arg{ .name = "text"_fs,
                   .description = "words to print"_fs,
                   .optional = true,
                   .variadic = true },
            = Flag{ .name = "no-newline"_fs,
                    .short_name = 'n',
                    .description = "do not print the trailing newline"_fs }
        ]] static auto echo(const Arguments& args, CommandIO& io) -> CallbackResult
        {
            for (std::size_t index{}; index < args.size(); ++index) {
                if (index != 0U) {
                    io.output.put(' ');
                }
                io.output << args.at(index);
            }
            if (!args.hasFlag("no-newline")) {
                io.output.put('\n');
            }
            return io.output ? CallbackResult{ 0 } : callback_failure("cannot write command output");
        }

        [[
            = "Print lines containing a fixed text pattern."_fs,
            = Arg{ .name = "pattern"_fs, .description = "text to find"_fs },
            = Arg{ .name = "file"_fs,
                   .description = "files to search; standard input is used when omitted"_fs,
                   .optional = true,
                   .variadic = true },
            = Flag{ .name = "ignore-case"_fs,
                    .short_name = 'i',
                    .description = "ignore ASCII letter case"_fs },
            = Flag{ .name = "line-number"_fs,
                    .short_name = 'n',
                    .description = "prefix matching lines with their line number"_fs },
            = Flag{ .name = "invert-match"_fs,
                    .short_name = 'v',
                    .description = "print non-matching lines"_fs }
        ]] static auto grep(const Arguments& args, CommandIO& io) -> CallbackResult
        {
            const std::string_view pattern{ args.require("pattern") };
            const bool ignore_case{ args.hasFlag("ignore-case") };
            const bool invert{ args.hasFlag("invert-match") };
            const bool line_numbers{ args.hasFlag("line-number") };
            const auto search = [&](std::istream& input,
                                    std::optional<std::string_view> label,
                                    const fs::path& path) -> CallbackResult {
                bool selected_any{};
                std::size_t line_number{};
                std::string line;

                errno = 0;
                while (std::getline(input, line)) {
                    ++line_number;
                    const bool selected{ contains(line, pattern, ignore_case) != invert };
                    if (!selected) {
                        continue;
                    }

                    selected_any = true;
                    if (label) {
                        io.output << *label << ':';
                    }
                    if (line_numbers) {
                        io.output << line_number << ':';
                    }
                    io.output << line << '\n';
                    if (!io.output) {
                        return callback_failure("cannot write command output");
                    }
                }

                if (input.bad()) {
                    if (!path.empty()) {
                        return stream_error("cannot read", path);
                    }
                    return callback_failure("cannot read standard input");
                }
                return selected_any ? 0 : 1;
            };

            if (args.size() == 1U) {
                return search(io.input, std::nullopt, {});
            }

            const bool print_filename{ args.size() > 2U };
            bool selected_any{};
            for (std::size_t index{ 1U }; index < args.size(); ++index) {
                auto resolved{ filesystem::resolvePath(args.at(index), "file") };
                if (!resolved) {
                    return std::unexpected(resolved.error());
                }
                const fs::path path{ std::move(*resolved) };

                std::error_code error;
                const fs::file_status status{ fs::status(path, error) };
                if (error) {
                    return callback_failure("cannot access '" + path_text(path) + "': " + error.message());
                }
                if (!fs::is_regular_file(status)) {
                    return callback_failure("'" + path_text(path) + "' is not a regular file");
                }

                errno = 0;
                std::ifstream file{ path, std::ios::in | std::ios::binary };
                if (!file.is_open()) {
                    return stream_error("cannot open", path);
                }
                const auto result{ search(file,
                                          print_filename ? std::optional<std::string_view>{ args.at(index) }
                                                         : std::nullopt,
                                          path) };
                if (!result) {
                    return result;
                }
                if (*result == 0) {
                    selected_any = true;
                }
                file.clear();
                file.close();
                if (!file) {
                    return stream_error("cannot read", path);
                }
            }
            return selected_any ? 0 : 1;
        }

        [[
            = "Profile a quoted command: elapsed time and CPU time across all threads. "
              "CPU resolution on STM32 is 10 ms."_fs,
            = Arg{ .name = "command"_fs,
                   .description = "command or pipeline to run, quoted as one argument"_fs }
        ]] static auto time(const Arguments& args, CommandIO& io) -> CallbackResult
        {
            const auto wall_before{ std::chrono::steady_clock::now() };
            const auto cpu_before{ std::clock() };
            if (cpu_before == static_cast<std::clock_t>(-1)) {
                return callback_failure("CPU clock is unavailable");
            }
            const auto result{ registry().execute(args.require("command"), io.output) };
            const auto cpu_after{ std::clock() };
            const auto wall_after{ std::chrono::steady_clock::now() };
            if (cpu_after == static_cast<std::clock_t>(-1) || cpu_after < cpu_before) {
                return callback_failure("CPU clock could not measure the command");
            }
            const double cpu_ms{ 1000.0 * static_cast<double>(cpu_after - cpu_before) / CLOCKS_PER_SEC };
            const double elapsed_ms{
                std::chrono::duration<double, std::milli>{ wall_after - wall_before }.count()
            };
            std::format_to(std::ostreambuf_iterator<char>{ io.output },
                           "CPU (all threads): {:.3f} ms\nElapsed: {:.3f} ms\n",
                           cpu_ms,
                           elapsed_ms);
            if (!io.output) {
                return callback_failure("cannot write profiling output");
            }
            if (!result) {
                return callback_failure(result.message.empty() ? "empty command" : result.message,
                                        result.exit_code);
            }
            return result.exit_code;
        }

        [[= "Print thread stack information."_fs]] static auto stack(const Arguments&, CommandIO& io)
          -> CallbackResult
        {
#if defined(PNM_PLATFORM_LINUX)
            tx::linux::refresh_all_threads_stack_info();
#endif

            auto threads_info{ getAllThreadsInformation() };
            if (threads_info.empty()) {
                io.output << "No threads running...\n";
                return io.output ? CallbackResult{ 0 } : callback_failure("cannot write command output");
            }

            std::ranges::sort(threads_info, std::ranges::less{}, &ThreadInfo::prio);

            const std::size_t max_name_len{ std::ranges::fold_left(
              threads_info, NAME_HEADER.size(), [](std::size_t maximum, const ThreadInfo& thread) {
                return std::max(maximum, thread.name.size());
            }) };
            const std::array stack_widths{ PRIO_HEADER.size(), max_name_len,       AVAIL_HEADER.size(),
                                           CURR_HEADER.size(), PEAK_HEADER.size(), USAGE_HEADER.size(),
                                           BAR_WIDTH };

            writeBorder(io.output, TOP_LEFT, TOP_JUNCTION, TOP_RIGHT, stack_widths);
            writeRow(io.output,
                     PRIO_HEADER,
                     NAME_HEADER,
                     max_name_len,
                     AVAIL_HEADER,
                     CURR_HEADER,
                     PEAK_HEADER,
                     USAGE_HEADER,
                     BAR_HEADER);
            io.output.put('\n');

            for (const auto& thread : threads_info) {
                writeBorder(io.output, MID_LEFT, MID_JUNCTION, MID_RIGHT, stack_widths);
                writeThreadRow(io.output, thread, max_name_len);
                io.output.put('\n');
            }

            writeBorder(io.output, BOTTOM_LEFT, BOTTOM_JUNCTION, BOTTOM_RIGHT, stack_widths);

            return io.output ? CallbackResult{ 0 } : callback_failure("cannot write command output");
        }

        [[= "Print heap allocator information."_fs]] static auto heap(const Arguments&, CommandIO& io)
          -> CallbackResult
        {
            const HeapInfo heap_info{ getHeapInformation() };
            const std::size_t usage_capacity{ heap_info.capacity.value_or(heap_info.reserved) };
            const std::size_t usage_percent{ usage_capacity == 0U
                                               ? 0U
                                               : (heap_info.used * 100U) / usage_capacity };
            const std::size_t value_width{ std::max(
              { HEAP_VALUE_HEADER.size(),
                heap_info.allocator.size(),
                heap_info.capacity ? decimalWidth(*heap_info.capacity) : "dynamic"sv.size(),
                decimalWidth(heap_info.reserved),
                decimalWidth(heap_info.used),
                decimalWidth(heap_info.available),
                decimalWidth(heap_info.releasable),
                decimalWidth(heap_info.free_blocks),
                decimalWidth(heap_info.mapped_regions),
                decimalWidth(heap_info.mapped_bytes),
                decimalWidth(usage_percent) }) };
            const std::array heap_widths{ HEAP_METRIC_WIDTH, value_width, HEAP_BAR_WIDTH };

            writeBorder(io.output, TOP_LEFT, TOP_JUNCTION, TOP_RIGHT, heap_widths);
            writeHeapHeader(io.output, value_width);
            writeBorder(io.output, MID_LEFT, MID_JUNCTION, MID_RIGHT, heap_widths);

            std::size_t metric_row{};
            std::size_t bar_row{};
            const auto write_value = [&](std::string_view metric, const auto& value) {
                if (metric_row != 0U) {
                    writeHeapRowSeparator(io.output, value_width, usage_percent, bar_row);
                    ++bar_row;
                }
                writeHeapRow(io.output, metric, value, value_width, usage_percent, bar_row);
                ++metric_row;
                ++bar_row;
            };

            write_value(HEAP_ALLOCATOR, heap_info.allocator);
            if (heap_info.capacity) {
                write_value(HEAP_CAPACITY, *heap_info.capacity);
            }
            else {
                write_value(HEAP_CAPACITY, "dynamic"sv);
            }
            write_value(HEAP_RESERVED, heap_info.reserved);
            write_value(HEAP_USED, heap_info.used);
            write_value(HEAP_AVAILABLE, heap_info.available);
            write_value(HEAP_RELEASABLE, heap_info.releasable);
            write_value(HEAP_FREE_BLOCKS, heap_info.free_blocks);
            write_value(HEAP_MAPPED_REGIONS, heap_info.mapped_regions);
            write_value(HEAP_MAPPED_BYTES, heap_info.mapped_bytes);
            write_value(HEAP_USAGE, usage_percent);

            PNM_ASSERT(metric_row == HEAP_METRIC_ROW_COUNT && bar_row == HEAP_BAR_HEIGHT,
                       "Invalid heap table row count");
            writeBorder(io.output, BOTTOM_LEFT, BOTTOM_JUNCTION, BOTTOM_RIGHT, heap_widths);

            return io.output ? CallbackResult{ 0 } : callback_failure("cannot write command output");
        }
    }

    auto setup() -> void
    {
        static std::once_flag registration;
        std::call_once(registration, [] { register_commands(); });
    }
}
