#include "filesystem.hpp"

#include "Parser.hpp"

#include "pneumo/meta.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <ostream>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace cli::filesystem
{
    namespace commands
    {
        namespace
        {
            using namespace pnm::meta::string::literals;
            namespace fs = std::filesystem;

            constexpr std::size_t maximum_wildcard_matches{ 256U };

            using PathResult = std::expected<fs::path, CallbackError>;

            struct PatternComponent
            {
                std::string text;
                std::vector<std::uint8_t> wildcard_mask;
            };

            [[nodiscard]] auto current_dir() -> fs::path&
            {
                static fs::path directory{ [] {
                    std::error_code error;
                    auto initial{ fs::current_path(error) };
                    return error ? fs::path{ "/" } : initial.lexically_normal();
                }() };
                return directory;
            }

            [[nodiscard]] auto path_text(const fs::path& path) -> std::string
            {
                const auto text{ path.generic_string() };
                return text.empty() ? std::string{ "." } : text;
            }

            [[nodiscard]] auto resolve_path(std::string_view value, std::string_view argument_name)
              -> PathResult
            {
                if (value.empty()) {
                    return std::unexpected(CallbackError{
                      .message = std::string{ argument_name } + " must not be empty",
                      .exit_code = 2,
                    });
                }

                fs::path path{ std::string{ value } };
                if (path.is_relative()) {
                    path = current_dir() / path;
                }
                path = path.lexically_normal();
                if (path != path.root_path() && path.filename().empty()) {
                    path = path.parent_path();
                }
                return path;
            }

            [[nodiscard]] auto wildcard_match(std::string_view value,
                                              const PatternComponent& pattern) noexcept -> bool
            {
                std::size_t value_index{};
                std::size_t pattern_index{};
                std::size_t star_index{ std::string_view::npos };
                std::size_t star_value_index{};

                while (value_index < value.size()) {
                    if (pattern_index < pattern.text.size() && pattern.wildcard_mask[pattern_index] == 0U &&
                        pattern.text[pattern_index] == value[value_index]) {
                        ++value_index;
                        ++pattern_index;
                        continue;
                    }
                    if (pattern_index < pattern.text.size() && pattern.wildcard_mask[pattern_index] != 0U) {
                        star_index = pattern_index++;
                        star_value_index = value_index;
                        continue;
                    }
                    if (star_index != std::string_view::npos) {
                        pattern_index = star_index + 1U;
                        value_index = ++star_value_index;
                        continue;
                    }
                    return false;
                }

                while (pattern_index < pattern.text.size() && pattern.wildcard_mask[pattern_index] != 0U) {
                    ++pattern_index;
                }
                return pattern_index == pattern.text.size();
            }

            [[nodiscard]] auto wildcard_failure(std::string_view action,
                                                const fs::path& path,
                                                const std::error_code& error) -> WildcardResult
            {
                std::string message{ action };
                message += " '";
                message += path.generic_string();
                message += '\'';
                if (error) {
                    message += ": ";
                    message += error.message();
                }
                return std::unexpected(CallbackError{
                  .message = std::move(message),
                  .exit_code = 1,
                });
            }

            [[nodiscard]] auto expand_wildcards(std::string_view pattern,
                                                std::span<const std::uint8_t> wildcard_mask) -> WildcardResult
            {
                if (pattern.size() != wildcard_mask.size()) {
                    return std::unexpected(CallbackError{
                      .message = "invalid wildcard metadata",
                      .exit_code = 2,
                    });
                }

                const bool has_wildcard{ std::ranges::any_of(
                  wildcard_mask, [](std::uint8_t value) { return value != 0U; }) };
                if (!has_wildcard) {
                    return std::vector<std::string>{ std::string{ pattern } };
                }
                for (std::size_t index{}; index < pattern.size(); ++index) {
                    if (wildcard_mask[index] != 0U && pattern[index] != '*') {
                        return std::unexpected(CallbackError{
                          .message = "invalid wildcard metadata",
                          .exit_code = 2,
                        });
                    }
                }

                const bool absolute{ pattern.starts_with('/') };
                const bool require_directory{ pattern.size() > 1U && pattern.ends_with('/') };
                std::vector<PatternComponent> components;
                for (std::size_t begin{}; begin < pattern.size();) {
                    while (begin < pattern.size() && pattern[begin] == '/') {
                        ++begin;
                    }
                    if (begin == pattern.size()) {
                        break;
                    }

                    const std::size_t end{ pattern.find('/', begin) };
                    const std::size_t component_end{ end == std::string_view::npos ? pattern.size() : end };
                    components.push_back({
                      .text = std::string{ pattern.substr(begin, component_end - begin) },
                      .wildcard_mask =
                        std::vector<std::uint8_t>{ wildcard_mask.begin() + static_cast<std::ptrdiff_t>(begin),
                                                   wildcard_mask.begin() +
                                                     static_cast<std::ptrdiff_t>(component_end) },
                    });
                    begin = component_end;
                }

                std::vector<fs::path> candidates{ absolute ? fs::path{ "/" } : current_dir() };
                for (const auto& component : components) {
                    const bool component_has_wildcard{ std::ranges::any_of(
                      component.wildcard_mask, [](std::uint8_t value) { return value != 0U; }) };
                    if (!component_has_wildcard) {
                        for (auto& candidate : candidates) {
                            candidate /= component.text;
                        }
                        continue;
                    }

                    std::vector<fs::path> matches;
                    for (const auto& candidate : candidates) {
                        std::error_code error;
                        fs::directory_iterator iterator{ candidate, error };
                        if (error == std::errc::no_such_file_or_directory ||
                            error == std::errc::not_a_directory) {
                            continue;
                        }
                        if (error) {
                            return wildcard_failure("cannot expand wildcard in", candidate, error);
                        }

                        const fs::directory_iterator end;
                        while (iterator != end) {
                            const std::string name{ iterator->path().filename().generic_string() };
                            const bool hidden_without_explicit_dot{ name.starts_with('.') &&
                                                                    (component.text.empty() ||
                                                                     !component.text.starts_with('.')) };
                            if (!hidden_without_explicit_dot && wildcard_match(name, component)) {
                                if (matches.size() == maximum_wildcard_matches) {
                                    return std::unexpected(CallbackError{
                                      .message = "pattern '" + std::string{ pattern } +
                                                 "' matched more than " +
                                                 std::to_string(maximum_wildcard_matches) + " paths",
                                      .exit_code = 2,
                                    });
                                }
                                matches.push_back(iterator->path());
                            }
                            iterator.increment(error);
                            if (error) {
                                return wildcard_failure(
                                  "cannot continue wildcard expansion in", candidate, error);
                            }
                        }
                    }
                    candidates = std::move(matches);
                    if (candidates.empty()) {
                        break;
                    }
                }

                std::vector<std::string> result;
                for (auto& candidate : candidates) {
                    candidate = candidate.lexically_normal();
                    std::error_code error;
                    const fs::file_status status{ fs::status(candidate, error) };
                    if (error == std::errc::no_such_file_or_directory ||
                        error == std::errc::not_a_directory) {
                        continue;
                    }
                    if (error) {
                        return wildcard_failure("cannot inspect wildcard match", candidate, error);
                    }
                    if (!fs::exists(status) || (require_directory && !fs::is_directory(status))) {
                        continue;
                    }

                    fs::path displayed{ candidate };
                    if (!absolute) {
                        displayed = candidate.lexically_relative(current_dir());
                    }
                    result.push_back(displayed.generic_string());
                }

                std::ranges::sort(result);
                result.erase(std::unique(result.begin(), result.end()), result.end());
                if (result.empty()) {
                    result.push_back(std::string{ pattern });
                }
                return result;
            }

            [[nodiscard]] auto filesystem_failure(std::string_view action,
                                                  const fs::path& path,
                                                  const std::error_code& error) -> CallbackResult
            {
                std::string message{ action };
                message += " '";
                message += path_text(path);
                message += '\'';
                if (error) {
                    message += ": ";
                    message += error.message();
                }
                return callback_failure(std::move(message));
            }

            [[nodiscard]] auto is_not_found(const std::error_code& error) noexcept -> bool
            {
                return error == std::errc::no_such_file_or_directory;
            }

            [[nodiscard]] auto entry_name(const fs::path& path) -> std::string
            {
                const auto filename{ path.filename().generic_string() };
                return filename.empty() ? path_text(path) : filename;
            }

            [[nodiscard]] auto is_hidden(const fs::path& path) -> bool
            {
                return entry_name(path).starts_with('.');
            }

            [[nodiscard]] auto is_same_or_parent(const fs::path& possible_parent, const fs::path& child)
              -> bool
            {
                const fs::path parent{ possible_parent.lexically_normal() };
                const fs::path normalized_child{ child.lexically_normal() };
                auto parent_part{ parent.begin() };
                auto child_part{ normalized_child.begin() };
                for (; parent_part != parent.end() && child_part != normalized_child.end();
                     ++parent_part, ++child_part) {
                    if (*parent_part != *child_part) {
                        return false;
                    }
                }
                return parent_part == parent.end();
            }

            [[nodiscard]] auto print_entry(const fs::path& path, bool long_format, std::ostream& output)
              -> CallbackResult
            {
                if (!long_format) {
                    output << entry_name(path);
                    return 0;
                }

                std::error_code error;
                const fs::file_status status{ fs::symlink_status(path, error) };
                if (error) {
                    return filesystem_failure("cannot inspect", path, error);
                }

                char type{ '?' };
                std::uintmax_t size{};
                if (fs::is_directory(status)) {
                    type = 'd';
                }
                else if (fs::is_regular_file(status)) {
                    type = '-';
                    size = fs::file_size(path, error);
                    if (error) {
                        return filesystem_failure("cannot determine size of", path, error);
                    }
                }
                else if (fs::is_symlink(status)) {
                    type = 'l';
                }

                output << type << ' ' << size << '\t' << entry_name(path);
                return 0;
            }

            [[nodiscard]] auto copy_path(const fs::path& source,
                                         const fs::path& destination,
                                         bool recursive,
                                         bool overwrite) -> CallbackResult
            {
                std::error_code error;
                const fs::file_status source_status{ fs::status(source, error) };
                if (error) {
                    return filesystem_failure("cannot inspect", source, error);
                }

                if (fs::is_directory(source_status)) {
                    if (!recursive) {
                        return callback_failure("'" + path_text(source) +
                                                "' is a directory; use --recursive");
                    }

                    fs::copy_options options{ fs::copy_options::recursive };
                    if (overwrite) {
                        options |= fs::copy_options::overwrite_existing;
                    }
                    fs::copy(source, destination, options, error);
                }
                else if (fs::is_regular_file(source_status)) {
                    const auto options{ overwrite ? fs::copy_options::overwrite_existing
                                                  : fs::copy_options::none };
                    static_cast<void>(fs::copy_file(source, destination, options, error));
                }
                else {
                    return callback_failure("'" + path_text(source) + "' is not a regular file or directory");
                }

                return error ? filesystem_failure("cannot copy to", destination, error) : CallbackResult{ 0 };
            }

            [[nodiscard]] auto list_path(const fs::path& path,
                                         bool include_hidden,
                                         bool long_format,
                                         bool one_per_line,
                                         bool print_heading,
                                         std::ostream& output) -> CallbackResult
            {
                std::error_code error;
                const fs::file_status status{ fs::status(path, error) };
                if (error) {
                    return filesystem_failure("cannot access", path, error);
                }

                if (!fs::is_directory(status)) {
                    auto result{ print_entry(path, long_format, output) };
                    if (!result) {
                        return result;
                    }
                    output << '\n';
                    return 0;
                }

                if (print_heading) {
                    output << path_text(path) << ":\n";
                }

                std::vector<fs::path> entries;
                fs::directory_iterator iterator{ path, error };
                const fs::directory_iterator end;
                while (!error && iterator != end) {
                    if (include_hidden || !is_hidden(iterator->path())) {
                        entries.push_back(iterator->path());
                    }
                    iterator.increment(error);
                }
                if (error) {
                    return filesystem_failure("cannot list", path, error);
                }

                std::ranges::sort(entries, {}, [](const fs::path& entry) { return entry_name(entry); });
                for (std::size_t index{}; index < entries.size(); ++index) {
                    auto result{ print_entry(entries[index], long_format, output) };
                    if (!result) {
                        return result;
                    }
                    output << (one_per_line || index + 1U == entries.size() ? '\n' : ' ');
                }
                return 0;
            }

            [[nodiscard]] auto touch_path(const fs::path& path, bool no_create) -> CallbackResult
            {
                std::error_code error;
                const fs::file_status status{ fs::status(path, error) };
                const bool missing{ is_not_found(error) || (!error && !fs::exists(status)) };
                if (error && !is_not_found(error)) {
                    return filesystem_failure("cannot inspect", path, error);
                }

                if (!missing) {
                    if (fs::is_directory(status)) {
                        return callback_failure("'" + path_text(path) + "' is a directory");
                    }
                    fs::last_write_time(path, fs::file_time_type::clock::now(), error);
                    return error ? filesystem_failure("cannot update", path, error) : CallbackResult{ 0 };
                }
                if (no_create) {
                    return 0;
                }

                const fs::path parent{ path.parent_path() };
                if (!parent.empty() && !fs::is_directory(parent, error)) {
                    if (error) {
                        return filesystem_failure("cannot access parent directory", parent, error);
                    }
                    return callback_failure("parent directory '" + path_text(parent) + "' does not exist");
                }

                errno = 0;
                std::ofstream file{ path, std::ios::out | std::ios::app | std::ios::binary };
                if (!file.is_open()) {
                    return filesystem_failure(
                      "cannot create", path, { errno == 0 ? EIO : errno, std::generic_category() });
                }
                file.close();
                if (!file) {
                    return filesystem_failure(
                      "cannot close", path, { errno == 0 ? EIO : errno, std::generic_category() });
                }
                return 0;
            }

            [[nodiscard]] auto create_directory_path(const fs::path& path, bool parents) -> CallbackResult
            {
                std::error_code error;
                const bool created{ parents ? fs::create_directories(path, error)
                                            : fs::create_directory(path, error) };
                if (error) {
                    return filesystem_failure("cannot create directory", path, error);
                }
                if (!created && !(parents && fs::is_directory(path, error))) {
                    return callback_failure("directory '" + path_text(path) + "' already exists");
                }
                if (error) {
                    return filesystem_failure("cannot inspect", path, error);
                }
                return 0;
            }

            [[nodiscard]] auto remove_empty_directory(const fs::path& path) -> CallbackResult
            {
                if (path.lexically_normal() == current_dir().lexically_normal()) {
                    return callback_failure("cannot remove the current working directory");
                }

                std::error_code error;
                const fs::file_status status{ fs::symlink_status(path, error) };
                if (error) {
                    return filesystem_failure("cannot access", path, error);
                }
                if (!fs::is_directory(status)) {
                    return callback_failure("'" + path_text(path) + "' is not a directory");
                }
                if (!fs::remove(path, error)) {
                    return error ? filesystem_failure("cannot remove directory", path, error)
                                 : callback_failure("directory '" + path_text(path) + "' was not removed");
                }
                return 0;
            }

            [[nodiscard]] auto remove_path(const fs::path& path, bool recursive, bool force) -> CallbackResult
            {
                std::error_code error;
                const fs::file_status status{ fs::symlink_status(path, error) };
                const bool missing{ is_not_found(error) || (!error && !fs::exists(status)) };
                if (missing) {
                    return force ? CallbackResult{ 0 }
                                 : callback_failure("'" + path_text(path) + "' does not exist");
                }
                if (error) {
                    return filesystem_failure("cannot access", path, error);
                }

                if (fs::is_directory(status)) {
                    if (!recursive) {
                        return callback_failure("'" + path_text(path) + "' is a directory; use --recursive");
                    }
                    if (is_same_or_parent(path, current_dir())) {
                        return callback_failure(
                          "refusing to remove the current working directory or its parent");
                    }
                    static_cast<void>(fs::remove_all(path, error));
                }
                else {
                    static_cast<void>(fs::remove(path, error));
                }
                return error ? filesystem_failure("cannot remove", path, error) : CallbackResult{ 0 };
            }

            [[nodiscard]] auto print_file(const fs::path& path, std::ostream& output) -> CallbackResult
            {
                std::error_code error;
                const fs::file_status status{ fs::status(path, error) };
                if (error) {
                    return filesystem_failure("cannot access", path, error);
                }
                if (!fs::is_regular_file(status)) {
                    return callback_failure("'" + path_text(path) + "' is not a regular file");
                }

                errno = 0;
                std::ifstream file{ path, std::ios::in | std::ios::binary };
                if (!file.is_open()) {
                    return filesystem_failure(
                      "cannot open", path, { errno == 0 ? EIO : errno, std::generic_category() });
                }

                std::array<char, 256U> buffer{};
                while (file.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) ||
                       file.gcount() > 0) {
                    output.write(buffer.data(), file.gcount());
                    if (!output) {
                        return callback_failure("cannot write file contents to command output");
                    }
                }
                if (file.bad()) {
                    return filesystem_failure(
                      "cannot read", path, { errno == 0 ? EIO : errno, std::generic_category() });
                }
                return 0;
            }
        }

        [[
            = "Print the current working directory."_fs,
            = Flag{ .name = "logical"_fs, .short_name = 'L', .description = "print the logical path"_fs },
            = Flag{ .name = "physical"_fs, .short_name = 'P', .description = "resolve symbolic links"_fs }
        ]] static auto pwd(const Arguments& args, std::ostream& output) -> CallbackResult
        {
            if (args.hasFlag("logical") && args.hasFlag("physical")) {
                return callback_failure("--logical and --physical are mutually exclusive", 2);
            }

            fs::path path{ current_dir() };
            if (args.hasFlag("physical")) {
                std::error_code error;
                path = fs::canonical(path, error);
                if (error) {
                    return filesystem_failure("cannot resolve", current_dir(), error);
                }
            }

            output << path.generic_string() << '\n';
            return 0;
        }

        [[
            = "Change the current working directory."_fs,
            = Arg{ .name = "directory"_fs, .description = "the new working directory"_fs }
        ]] static auto cd(const Arguments& args, std::ostream&) -> CallbackResult
        {
            auto path{ resolve_path(args.require("directory"), "directory") };
            if (!path) {
                return std::unexpected(path.error());
            }

            std::error_code error;
            const fs::file_status status{ fs::status(*path, error) };
            if (error) {
                return filesystem_failure("cannot access", *path, error);
            }
            if (!fs::is_directory(status)) {
                return callback_failure("'" + path_text(*path) + "' is not a directory");
            }

            current_dir() = path->lexically_normal();
            return 0;
        }

        [[
            = "List a file or directory (the current directory by default)."_fs,
            = Arg{ .name = "path"_fs,
                   .description = "files or directories to list"_fs,
                   .optional = true,
                   .variadic = true },
            = Flag{ .name = "all"_fs, .short_name = 'a', .description = "include hidden entries"_fs },
            = Flag{ .name = "long"_fs, .short_name = 'l', .description = "use a long listing format"_fs },
            = Flag{ .name = "one"_fs, .short_name = '1', .description = "print one entry per line"_fs }
        ]] static auto ls(const Arguments& args, std::ostream& output) -> CallbackResult
        {
            const bool long_format{ args.hasFlag("long") };
            const bool one_per_line{ long_format || args.hasFlag("one") };
            if (args.empty()) {
                return list_path(
                  current_dir(), args.hasFlag("all"), long_format, one_per_line, false, output);
            }

            for (std::size_t index{}; index < args.size(); ++index) {
                auto resolved{ resolve_path(args.at(index), "path") };
                if (!resolved) {
                    return std::unexpected(resolved.error());
                }
                auto result{ list_path(
                  *resolved, args.hasFlag("all"), long_format, one_per_line, args.size() > 1U, output) };
                if (!result) {
                    return result;
                }
            }
            return 0;
        }

        [[
            = "Create a file or update its modification time."_fs,
            = Arg{ .name = "file"_fs, .description = "files to create or update"_fs, .variadic = true },
            = Flag{ .name = "no-create"_fs,
                    .short_name = 'c',
                    .description = "do not create a missing file"_fs }
        ]] static auto touch(const Arguments& args, std::ostream&) -> CallbackResult
        {
            for (std::size_t index{}; index < args.size(); ++index) {
                auto path{ resolve_path(args.at(index), "file") };
                if (!path) {
                    return std::unexpected(path.error());
                }
                auto result{ touch_path(*path, args.hasFlag("no-create")) };
                if (!result) {
                    return result;
                }
            }
            return 0;
        }

        [[
            = "Create a directory."_fs,
            = Arg{ .name = "directory"_fs, .description = "directories to create"_fs, .variadic = true },
            = Flag{ .name = "parents"_fs,
                    .short_name = 'p',
                    .description = "create missing parent directories"_fs }
        ]] static auto mkdir(const Arguments& args, std::ostream&) -> CallbackResult
        {
            for (std::size_t index{}; index < args.size(); ++index) {
                auto path{ resolve_path(args.at(index), "directory") };
                if (!path) {
                    return std::unexpected(path.error());
                }
                auto result{ create_directory_path(*path, args.hasFlag("parents")) };
                if (!result) {
                    return result;
                }
            }
            return 0;
        }

        [[
            = "Remove an empty directory."_fs,
            = Arg{ .name = "directory"_fs, .description = "empty directories to remove"_fs, .variadic = true }
        ]] static auto rmdir(const Arguments& args, std::ostream&) -> CallbackResult
        {
            for (std::size_t index{}; index < args.size(); ++index) {
                auto path{ resolve_path(args.at(index), "directory") };
                if (!path) {
                    return std::unexpected(path.error());
                }
                auto result{ remove_empty_directory(*path) };
                if (!result) {
                    return result;
                }
            }
            return 0;
        }

        [[
            = "Remove a file or directory."_fs,
            = Arg{ .name = "path"_fs, .description = "files or directories to remove"_fs, .variadic = true },
            = Flag{ .name = "recursive"_fs,
                    .short_name = 'r',
                    .description = "remove directories recursively"_fs },
            = Flag{ .name = "force"_fs, .short_name = 'f', .description = "ignore a missing path"_fs }
        ]] static auto rm(const Arguments& args, std::ostream&) -> CallbackResult
        {
            for (std::size_t index{}; index < args.size(); ++index) {
                auto path{ resolve_path(args.at(index), "path") };
                if (!path) {
                    return std::unexpected(path.error());
                }
                auto result{ remove_path(*path, args.hasFlag("recursive"), args.hasFlag("force")) };
                if (!result) {
                    return result;
                }
            }
            return 0;
        }

        [[
            = "Copy a file or directory."_fs,
            = Arg{ .name = "source"_fs, .description = "source paths"_fs, .variadic = true },
            = Arg{ .name = "destination"_fs, .description = "the destination path"_fs },
            = Flag{ .name = "recursive"_fs,
                    .short_name = 'r',
                    .description = "copy directories recursively"_fs },
            = Flag{ .name = "force"_fs, .short_name = 'f', .description = "overwrite existing files"_fs }
        ]] static auto cp(const Arguments& args, std::ostream&) -> CallbackResult
        {
            auto destination_root{ resolve_path(args.require("destination"), "destination") };
            if (!destination_root) {
                return std::unexpected(destination_root.error());
            }

            const std::size_t source_count{ args.size() - 1U };
            if (source_count > 1U) {
                std::error_code error;
                const fs::file_status destination_status{ fs::status(*destination_root, error) };
                if (error) {
                    return filesystem_failure(
                      "cannot access multi-source destination", *destination_root, error);
                }
                if (!fs::is_directory(destination_status)) {
                    return callback_failure("multi-source destination '" + path_text(*destination_root) +
                                            "' is not a directory");
                }
            }

            for (std::size_t index{}; index < source_count; ++index) {
                auto source{ resolve_path(args.at(index), "source") };
                if (!source) {
                    return std::unexpected(source.error());
                }

                std::error_code error;
                const fs::file_status source_status{ fs::status(*source, error) };
                if (error) {
                    return filesystem_failure("cannot access", *source, error);
                }
                if (!fs::exists(source_status)) {
                    return callback_failure("source '" + path_text(*source) + "' does not exist");
                }

                fs::path destination{ *destination_root };
                if (fs::is_directory(destination, error)) {
                    destination /= source->filename();
                }
                else if (error && !is_not_found(error)) {
                    return filesystem_failure("cannot inspect", destination, error);
                }
                error.clear();

                if (fs::exists(destination, error)) {
                    if (fs::equivalent(*source, destination, error) && !error) {
                        return callback_failure("source and destination are the same path");
                    }
                    if (!args.hasFlag("force")) {
                        return callback_failure("destination '" + path_text(destination) +
                                                "' already exists");
                    }
                }
                if (error) {
                    return filesystem_failure("cannot inspect", destination, error);
                }

                auto result{ copy_path(
                  *source, destination, args.hasFlag("recursive"), args.hasFlag("force")) };
                if (!result) {
                    return result;
                }
            }
            return 0;
        }

        [[
            = "Move or rename a file or directory."_fs,
            = Arg{ .name = "source"_fs, .description = "source paths"_fs, .variadic = true },
            = Arg{ .name = "destination"_fs, .description = "the destination path"_fs },
            = Flag{ .name = "force"_fs,
                    .short_name = 'f',
                    .description = "replace an existing destination"_fs }
        ]] static auto mv(const Arguments& args, std::ostream&) -> CallbackResult
        {
            auto destination_root{ resolve_path(args.require("destination"), "destination") };
            if (!destination_root) {
                return std::unexpected(destination_root.error());
            }

            const std::size_t source_count{ args.size() - 1U };
            if (source_count > 1U) {
                std::error_code error;
                const fs::file_status destination_status{ fs::status(*destination_root, error) };
                if (error) {
                    return filesystem_failure(
                      "cannot access multi-source destination", *destination_root, error);
                }
                if (!fs::is_directory(destination_status)) {
                    return callback_failure("multi-source destination '" + path_text(*destination_root) +
                                            "' is not a directory");
                }
            }

            for (std::size_t index{}; index < source_count; ++index) {
                auto source{ resolve_path(args.at(index), "source") };
                if (!source) {
                    return std::unexpected(source.error());
                }

                std::error_code error;
                const fs::file_status source_status{ fs::symlink_status(*source, error) };
                if (error) {
                    return filesystem_failure("cannot access", *source, error);
                }

                fs::path destination{ *destination_root };
                if (fs::is_directory(destination, error)) {
                    destination /= source->filename();
                }
                else if (error && !is_not_found(error)) {
                    return filesystem_failure("cannot inspect", destination, error);
                }
                error.clear();

                if (fs::exists(destination, error)) {
                    if (fs::equivalent(*source, destination, error) && !error) {
                        continue;
                    }
                    if (!args.hasFlag("force")) {
                        return callback_failure("destination '" + path_text(destination) +
                                                "' already exists");
                    }
                }
                if (error) {
                    return filesystem_failure("cannot inspect", destination, error);
                }

                fs::rename(*source, destination, error);
                if (!error) {
                    continue;
                }
                if (error != std::errc::cross_device_link) {
                    return filesystem_failure("cannot move to", destination, error);
                }

                auto copied{ copy_path(*source, destination, true, args.hasFlag("force")) };
                if (!copied) {
                    return copied;
                }
                error.clear();
                if (fs::is_directory(source_status)) {
                    static_cast<void>(fs::remove_all(*source, error));
                }
                else {
                    static_cast<void>(fs::remove(*source, error));
                }
                if (error) {
                    return filesystem_failure("copied but cannot remove source", *source, error);
                }
            }
            return 0;
        }

        [[
            = "Print a file's contents."_fs,
            = Arg{ .name = "file"_fs, .description = "files to print"_fs, .variadic = true }
        ]] static auto cat(const Arguments& args, std::ostream& output) -> CallbackResult
        {
            for (std::size_t index{}; index < args.size(); ++index) {
                auto path{ resolve_path(args.at(index), "file") };
                if (!path) {
                    return std::unexpected(path.error());
                }
                auto result{ print_file(*path, output) };
                if (!result) {
                    return result;
                }
            }
            return 0;
        }
    }

    auto resolvePath(std::string_view value, std::string_view argument_name) -> PathResult
    {
        return commands::resolve_path(value, argument_name);
    }

    auto expandWildcards(std::string_view pattern, std::span<const std::uint8_t> wildcard_mask)
      -> WildcardResult
    {
        return commands::expand_wildcards(pattern, wildcard_mask);
    }

    auto setup() -> void
    {
        static std::once_flag registration;
        std::call_once(registration, [] { register_commands(); });
    }
}
