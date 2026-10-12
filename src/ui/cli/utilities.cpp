#include "utilities.hpp"

#include "Parser.hpp"
#include "common/diagnostics/Memory.hpp"
#include "common/icc/catalog/services.hpp"
#include "filesystem.hpp"

#include "pneumo/meta.hpp"

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

namespace ui::cli::utilities
{
    namespace commands
    {
        namespace
        {
            constexpr std::size_t DECIMAL_BASE{ 10U };
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

            using diagnostics::HeapInfo;
            using diagnostics::ThreadInfo;
            using MemoryService = pnm::msg::Service<diagnostics::MemoryQuery, diagnostics::MemorySnapshot>;

            struct Context
            {
                Parser* parser;
                MemoryService memory;
                std::stop_token stop;
            };

            std::string_view service_error(pnm::msg::ServiceError error)
            {
                using Error = pnm::msg::ServiceError;
                switch (error) {
                    case Error::Unavailable:
                        return "service unavailable";
                    case Error::Busy:
                        return "service busy";
                    case Error::Timeout:
                        return "request timed out";
                    case Error::Cancelled:
                        return "request cancelled";
                    case Error::HandlerFailed:
                        return "collection failed";
                    case Error::TransportError:
                        return "ICC connection failed";
                }
                return "unknown service error";
            }

            auto get_memory_information(const Arguments& args,
                                        const Context& context,
                                        diagnostics::MemoryQuery query)
              -> std::expected<diagnostics::MemorySnapshot, CallbackError>
            {
                const auto controller{ args.require("controller") };
                if (controller == "ui") {
                    return diagnostics::collectMemory(query);
                }
                if (controller != "io") {
                    return std::unexpected(CallbackError{ "controller must be 'ui' or 'io'" });
                }
                // Wait only on the CLI thread. ICC workers remain free to route
                // the request/reply; shutdown cancels the outstanding request.
                auto call{ context.memory.request(query, std::chrono::seconds{ 2 }, context.stop) };
                auto result{ call.get() };
                if (!result) {
                    return std::unexpected(
                      CallbackError{ "IO diagnostics: " + std::string{ service_error(result.error()) } });
                }
                if (result->query != query || result->stack.count > result->stack.threads.size() ||
                    result->stack.total < result->stack.count) {
                    return std::unexpected(CallbackError{ "IO diagnostics: invalid response" });
                }
                return std::move(*result);
            }

            std::string_view thread_name(const ThreadInfo& info)
            {
                const auto end{ std::ranges::find(info.name, '\0') };
                return { info.name.data(), static_cast<std::size_t>(end - info.name.begin()) };
            }

            void write_controller(std::ostream& output, std::string_view controller, bool shared_runtime)
            {
                output << "Controller: " << (controller == "io" ? "IO" : "UI") << '\n';
                if (shared_runtime) {
                    output << "Linux: shared process heap and ThreadX threads (UI + IO).\n";
                }
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
            constexpr std::string_view BAR_USED{ "\u2588"sv };
            constexpr std::string_view BAR_FREE{ "\u2591"sv };

            constexpr std::string_view PRIO_HEADER{ "Prio"sv };
            constexpr std::string_view NAME_HEADER{ "Name"sv };
            constexpr std::string_view AVAIL_HEADER{ "Available [bytes]"sv };
            constexpr std::string_view CURR_HEADER{ "Current [bytes]"sv };
            constexpr std::string_view PEAK_HEADER{ "Peak [bytes]"sv };
            constexpr std::string_view USAGE_HEADER{ "Usage [%]"sv };
            constexpr std::string_view BAR_HEADER{ "Stack-Usage Bar"sv };

            constexpr std::size_t PRIO_WIDTH{ PRIO_HEADER.size() };
            constexpr std::size_t AVAIL_WIDTH{ AVAIL_HEADER.size() };
            constexpr std::size_t CURR_WIDTH{ CURR_HEADER.size() };
            constexpr std::size_t PEAK_WIDTH{ PEAK_HEADER.size() };
            constexpr std::size_t USAGE_WIDTH{ USAGE_HEADER.size() };

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

            auto write_repeated(std::ostream& output, std::string_view value, std::size_t count) -> void
            {
                for (std::size_t index{}; index < count; ++index) {
                    output << value;
                }
            }

            // Border glyphs are deliberately ordered left, junction and right.
            // NOLINTBEGIN(bugprone-easily-swappable-parameters)
            auto write_border(std::ostream& output,
                              std::string_view left,
                              std::string_view junction,
                              std::string_view right,
                              std::span<const std::size_t> widths) -> void
            {
                // NOLINTEND(bugprone-easily-swappable-parameters)
                output << left;
                for (std::size_t index{}; index < widths.size(); ++index) {
                    write_repeated(output, HORIZONTAL, widths[index] + 2U);
                    output << (index + 1U == widths.size() ? right : junction);
                }
                output.put('\n');
            }

            auto write_row(std::ostream& output,
                           const auto& priority,
                           std::string_view name,
                           std::size_t name_width,
                           const auto& available,
                           const auto& current,
                           const auto& peak,
                           const auto& usage,
                           std::string_view bar) -> void
            {
                // Runtime format parsing avoids Clang/libstdc++'s dynamic-width consteval issue.
                std::vformat_to(std::ostreambuf_iterator<char>{ output },
                                "\u2502 {:>{}} \u2502 {:<{}} \u2502 {:>{}} \u2502 {:>{}} "
                                "\u2502 {:>{}} \u2502 {:>{}} \u2502 {:<{}} \u2502",
                                std::make_format_args(priority,
                                                      PRIO_WIDTH,
                                                      name,
                                                      name_width,
                                                      available,
                                                      AVAIL_WIDTH,
                                                      current,
                                                      CURR_WIDTH,
                                                      peak,
                                                      PEAK_WIDTH,
                                                      usage,
                                                      USAGE_WIDTH,
                                                      bar,
                                                      BAR_WIDTH));
            }

            auto write_thread_row(std::ostream& output, const ThreadInfo& thread, std::size_t name_width)
              -> void
            {
                const auto usage_percent{ thread.stack_size == 0U
                                            ? 0U
                                            : (static_cast<std::uint64_t>(thread.peak_stack_usage) * 100U) /
                                                thread.stack_size };
                const auto name{ thread_name(thread) };
                const auto bar_length{ static_cast<std::size_t>(
                  std::min<std::uint64_t>((usage_percent * BAR_WIDTH) / 100U, BAR_WIDTH)) };

                std::vformat_to(std::ostreambuf_iterator<char>{ output },
                                "\u2502 {:>{}} \u2502 {:<{}} \u2502 {:>{}} \u2502 {:>{}} "
                                "\u2502 {:>{}} \u2502 {:>{}} \u2502 ",
                                std::make_format_args(thread.prio,
                                                      PRIO_WIDTH,
                                                      name,
                                                      name_width,
                                                      thread.stack_size,
                                                      AVAIL_WIDTH,
                                                      thread.current_stack_usage,
                                                      CURR_WIDTH,
                                                      thread.peak_stack_usage,
                                                      PEAK_WIDTH,
                                                      usage_percent,
                                                      USAGE_WIDTH));
                write_repeated(output, BAR_USED, bar_length);
                write_repeated(output, BAR_FREE, BAR_WIDTH - bar_length);
                output << " \u2502";
            }

            [[nodiscard]] auto decimal_width(std::uint64_t value) -> std::size_t
            {
                std::size_t width{ 1U };
                while (value >= DECIMAL_BASE) {
                    value /= DECIMAL_BASE;
                    ++width;
                }
                return width;
            }

            auto write_heap_header(std::ostream& output, std::size_t value_width) -> void
            {
                std::vformat_to(std::ostreambuf_iterator<char>{ output },
                                "\u2502 {:<{}} \u2502 {:>{}} \u2502 {:<{}} \u2502",
                                std::make_format_args(HEAP_METRIC_HEADER,
                                                      HEAP_METRIC_WIDTH,
                                                      HEAP_VALUE_HEADER,
                                                      value_width,
                                                      HEAP_BAR,
                                                      HEAP_BAR_WIDTH));
                output.put('\n');
            }

            // These formatting helpers take the table dimensions and row in documented order.
            // NOLINTBEGIN(bugprone-easily-swappable-parameters)
            [[nodiscard]] auto heap_bar_used(std::size_t usage_percent, std::size_t row) -> bool
            {
                // NOLINTEND(bugprone-easily-swappable-parameters)
                const std::size_t clamped_usage{ std::min(usage_percent, std::size_t{ 100U }) };
                const std::size_t used_rows{ (clamped_usage * HEAP_BAR_HEIGHT + 50U) / 100U };
                return row >= HEAP_BAR_HEIGHT - used_rows;
            }

            auto write_heap_bar_cell(std::ostream& output, std::size_t usage_percent, std::size_t row) -> void
            {
                output.put(' ');
                write_repeated(
                  output, heap_bar_used(usage_percent, row) ? BAR_USED : BAR_FREE, HEAP_BAR_WIDTH);
                output << " \u2502\n";
            }

            // These formatting helpers take the table dimensions and row in documented order.
            // NOLINTBEGIN(bugprone-easily-swappable-parameters)
            auto write_heap_row_separator(std::ostream& output,
                                          std::size_t value_width,
                                          std::size_t usage_percent,
                                          std::size_t row) -> void
            {
                // NOLINTEND(bugprone-easily-swappable-parameters)
                output << MID_LEFT;
                write_repeated(output, HORIZONTAL, HEAP_METRIC_WIDTH + 2U);
                output << MID_JUNCTION;
                write_repeated(output, HORIZONTAL, value_width + 2U);
                output << MID_RIGHT;
                write_heap_bar_cell(output, usage_percent, row);
            }

            // These formatting helpers take the table dimensions and row in documented order.
            // NOLINTBEGIN(bugprone-easily-swappable-parameters)
            auto write_heap_row(std::ostream& output,
                                std::string_view metric,
                                const auto& value,
                                std::size_t value_width,
                                std::size_t usage_percent,
                                std::size_t row) -> void
            {
                // NOLINTEND(bugprone-easily-swappable-parameters)
                std::vformat_to(std::ostreambuf_iterator<char>{ output },
                                "\u2502 {:<{}} \u2502 {:>{}} \u2502",
                                std::make_format_args(metric, HEAP_METRIC_WIDTH, value, value_width));
                write_heap_bar_cell(output, usage_percent, row);
            }
            struct SearchOptions
            {
                std::string_view pattern;
                bool ignore_case{}, invert{}, line_numbers{};
            };
            auto search_stream(std::istream& input,
                               std::optional<std::string_view> label,
                               const fs::path& path,
                               std::ostream& output,
                               const SearchOptions& options) -> CallbackResult
            {
                bool selected_any{};
                std::size_t line_number{};
                std::string line;

                errno = 0;
                while (std::getline(input, line)) {
                    ++line_number;
                    const bool selected{ contains(line, options.pattern, options.ignore_case) !=
                                         options.invert };
                    if (!selected) {
                        continue;
                    }

                    selected_any = true;
                    if (label) {
                        output << *label << ':';
                    }
                    if (options.line_numbers) {
                        output << line_number << ':';
                    }
                    output << line << '\n';
                    if (!output) {
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

            if (args.size() == 1U) {
                return search_stream(io.input,
                                     std::nullopt,
                                     {},
                                     io.output,
                                     { .pattern = pattern,
                                       .ignore_case = ignore_case,
                                       .invert = invert,
                                       .line_numbers = line_numbers });
            }

            const bool print_filename{ args.size() > 2U };
            bool selected_any{};
            for (std::size_t index{ 1U }; index < args.size(); ++index) {
                auto resolved{ filesystem::resolve_path(args.at(index), "file") };
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
                const auto result{ search_stream(
                  file,
                  print_filename ? std::optional<std::string_view>{ args.at(index) } : std::nullopt,
                  path,
                  io.output,
                  { .pattern = pattern,
                    .ignore_case = ignore_case,
                    .invert = invert,
                    .line_numbers = line_numbers }) };
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
        ]] static auto time(const Arguments& args, CommandIO& io, const Context& context) -> CallbackResult
        {
            const auto wall_before{ std::chrono::steady_clock::now() };
            const auto cpu_before{ std::clock() };
            if (cpu_before == static_cast<std::clock_t>(-1)) {
                return callback_failure("CPU clock is unavailable");
            }
            const auto result{ context.parser->execute(args.require("command"), io.output) };
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

        [[
            = "Print thread stack information for a controller."_fs,
            = Arg{ .name = "controller"_fs, .description = "ui (local) or io (via ICC)"_fs }
        ]] static auto stack(const Arguments& args, CommandIO& io, const Context& context) -> CallbackResult
        {
            auto result{ get_memory_information(args, context, diagnostics::MemoryQuery::Stack) };
            if (!result) {
                return std::unexpected(result.error());
            }
            write_controller(io.output, args.require("controller"), result->shared_runtime);
            auto threads_info{ std::span{ result->stack.threads }.first(result->stack.count) };
            if (threads_info.empty()) {
                io.output << "No threads running...\n";
                return io.output ? CallbackResult{ 0 } : callback_failure("cannot write command output");
            }

            std::ranges::sort(threads_info, std::ranges::less{}, &ThreadInfo::prio);

            const std::size_t max_name_len{ std::ranges::fold_left(
              threads_info, NAME_HEADER.size(), [](std::size_t maximum, const ThreadInfo& thread) {
                return std::max(maximum, thread_name(thread).size());
            }) };
            const std::array stack_widths{ PRIO_WIDTH, max_name_len, AVAIL_WIDTH, CURR_WIDTH,
                                           PEAK_WIDTH, USAGE_WIDTH,  BAR_WIDTH };

            write_border(io.output, TOP_LEFT, TOP_JUNCTION, TOP_RIGHT, stack_widths);
            write_row(io.output,
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
                write_border(io.output, MID_LEFT, MID_JUNCTION, MID_RIGHT, stack_widths);
                write_thread_row(io.output, thread, max_name_len);
                io.output.put('\n');
            }

            write_border(io.output, BOTTOM_LEFT, BOTTOM_JUNCTION, BOTTOM_RIGHT, stack_widths);
            io.output << "Current: sampled/saved thread stack usage; peak: recorded high-water mark.\n";
            if (result->stack.total > result->stack.count) {
                io.output << "Snapshot limited to " << result->stack.count << " of " << result->stack.total
                          << " threads.\n";
            }

            return io.output ? CallbackResult{ 0 } : callback_failure("cannot write command output");
        }

        [[
            = "Print heap allocator information for a controller."_fs,
            = Arg{ .name = "controller"_fs, .description = "ui (local) or io (via ICC)"_fs }
        ]] static auto heap(const Arguments& args, CommandIO& io, const Context& context) -> CallbackResult
        {
            auto result{ get_memory_information(args, context, diagnostics::MemoryQuery::Heap) };
            if (!result) {
                return std::unexpected(result.error());
            }
            write_controller(io.output, args.require("controller"), result->shared_runtime);
            const HeapInfo& heap_info{ result->heap };
            const std::string_view allocator{ heap_info.allocator == diagnostics::Allocator::Glibc
                                                ? "glibc"
                                                : "Newlib" };
            const auto usage_capacity{ heap_info.capacity != 0U ? heap_info.capacity : heap_info.reserved };
            const auto usage_percent{ usage_capacity == 0U ? 0U : (heap_info.used * 100U) / usage_capacity };
            const std::size_t value_width{ std::max(
              { HEAP_VALUE_HEADER.size(),
                allocator.size(),
                heap_info.capacity ? decimal_width(heap_info.capacity) : "dynamic"sv.size(),
                decimal_width(heap_info.reserved),
                decimal_width(heap_info.used),
                decimal_width(heap_info.available),
                decimal_width(heap_info.releasable),
                decimal_width(heap_info.free_blocks),
                decimal_width(heap_info.mapped_regions),
                decimal_width(heap_info.mapped_bytes),
                decimal_width(usage_percent) }) };
            const std::array heap_widths{ HEAP_METRIC_WIDTH, value_width, HEAP_BAR_WIDTH };

            write_border(io.output, TOP_LEFT, TOP_JUNCTION, TOP_RIGHT, heap_widths);
            write_heap_header(io.output, value_width);
            write_border(io.output, MID_LEFT, MID_JUNCTION, MID_RIGHT, heap_widths);

            std::size_t metric_row{};
            std::size_t bar_row{};
            const auto write_value = [&](std::string_view metric, const auto& value) {
                if (metric_row != 0U) {
                    write_heap_row_separator(io.output, value_width, usage_percent, bar_row);
                    ++bar_row;
                }
                write_heap_row(io.output, metric, value, value_width, usage_percent, bar_row);
                ++metric_row;
                ++bar_row;
            };

            write_value(HEAP_ALLOCATOR, allocator);
            if (heap_info.capacity) {
                write_value(HEAP_CAPACITY, heap_info.capacity);
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

            // Pneumo assertions implement failure reporting through a C macro.
            // NOLINTBEGIN(cppcoreguidelines-avoid-do-while,cppcoreguidelines-pro-type-vararg,modernize-use-std-print,readability-simplify-boolean-expr)
            PNM_ASSERT(metric_row == HEAP_METRIC_ROW_COUNT && bar_row == HEAP_BAR_HEIGHT,
                       "Invalid heap table row count");
            // NOLINTEND(cppcoreguidelines-avoid-do-while,cppcoreguidelines-pro-type-vararg,modernize-use-std-print,readability-simplify-boolean-expr)
            write_border(io.output, BOTTOM_LEFT, BOTTOM_JUNCTION, BOTTOM_RIGHT, heap_widths);

            return io.output ? CallbackResult{ 0 } : callback_failure("cannot write command output");
        }
    }

    void setup(Parser& parser, pnm::msg::Bus& bus, std::stop_token stop)
    {
        std::call_once(parser.m_utilitiesRegistration, [&parser, &bus, stop] {
            const commands::Context context{ &parser, bus.service<^^icc::Services::io_memory>(), stop };
            register_commands<^^commands>(parser, [context](auto function) -> Callback {
                return [context, function](const Arguments& args, CommandIO& io) -> CallbackResult {
                    if constexpr (std::invocable<decltype(function),
                                                 const Arguments&,
                                                 CommandIO&,
                                                 const commands::Context&>) {
                        return function(args, io, context);
                    }
                    else {
                        return function(args, io);
                    }
                };
            });
        });
    }
}
