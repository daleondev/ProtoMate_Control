#include "Parser.hpp"

#include "filesystem.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <exception>
#include <fstream>
#include <ostream>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <system_error>
#include <utility>

namespace
{
    constexpr std::size_t pipeline_capacity{ 32U * 1024U };

    enum class Quote
    {
        None,
        Single,
        Double,
    };

    enum class TokenType
    {
        Word,
        Pipe,
        Redirect,
        Append,
    };

    struct Token
    {
        TokenType type{ TokenType::Word };
        std::string text;
        std::vector<std::uint8_t> wildcard_mask;
    };

    struct Word
    {
        std::string text;
        std::vector<std::uint8_t> wildcard_mask;
    };

    struct Redirection
    {
        Word target;
        bool append{};
    };

    struct ParsedLine
    {
        std::vector<std::vector<Word>> stages;
        std::optional<Redirection> redirection;
    };

    class LimitedStringBuffer final : public std::streambuf
    {
      public:
        [[nodiscard]] auto take() noexcept -> std::string&& { return std::move(m_value); }
        [[nodiscard]] auto exceeded() const noexcept -> bool { return m_exceeded; }

      protected:
        auto xsputn(const char* data, std::streamsize count) -> std::streamsize override
        {
            if (count <= 0) {
                return 0;
            }

            const auto requested{ static_cast<std::size_t>(count) };
            const auto available{ pipeline_capacity - m_value.size() };
            const auto accepted{ std::min(requested, available) };
            m_value.append(data, accepted);
            m_exceeded = m_exceeded || accepted != requested;

            // Accept the complete write from the command's point of view. The
            // parser reports one precise overflow diagnostic after the stage.
            return count;
        }

        auto overflow(int_type character) -> int_type override
        {
            if (traits_type::eq_int_type(character, traits_type::eof())) {
                return traits_type::not_eof(character);
            }
            if (m_value.size() < pipeline_capacity) {
                m_value.push_back(traits_type::to_char_type(character));
            }
            else {
                m_exceeded = true;
            }
            return character;
        }

      private:
        std::string m_value;
        bool m_exceeded{};
    };

    [[nodiscard]] auto valid_name(std::string_view name) noexcept -> bool
    {
        return !name.empty() && std::ranges::all_of(name, [](unsigned char character) {
            return std::isalnum(character) != 0 || character == '_' || character == '-' || character == '.';
        });
    }

    [[nodiscard]] auto valid_command_name(std::string_view name) noexcept -> bool
    {
        // Command paths use single spaces; argument and flag names still use
        // valid_name. This permits independent motor/robot command modules.
        while (true) {
            const auto end{ name.find(' ') };
            if (!valid_name(name.substr(0, end)))
                return false;
            if (end == std::string_view::npos)
                return true;
            name.remove_prefix(end + 1U);
        }
    }

    [[nodiscard]] auto negative_number(std::string_view value) noexcept -> bool
    {
        double number{};
        const auto result{ std::from_chars(value.data(), value.data() + value.size(), number) };
        return result.ec == std::errc{} && result.ptr == value.data() + value.size();
    }

    [[nodiscard]] auto tokenize(std::string_view line) -> std::expected<std::vector<Token>, std::string>
    {
        std::vector<Token> tokens;
        std::string token;
        std::vector<std::uint8_t> wildcard_mask;
        Quote quote{ Quote::None };
        bool escaped{};
        bool token_started{};

        const auto finish_word = [&] {
            if (token_started) {
                tokens.push_back({
                  .type = TokenType::Word,
                  .text = std::move(token),
                  .wildcard_mask = std::move(wildcard_mask),
                });
                token.clear();
                wildcard_mask.clear();
                token_started = false;
            }
        };

        for (std::size_t index{}; index < line.size(); ++index) {
            const char character{ line[index] };
            if (escaped) {
                token.push_back(character);
                wildcard_mask.push_back(0U);
                token_started = true;
                escaped = false;
                continue;
            }

            if (quote != Quote::Single && character == '\\') {
                escaped = true;
                token_started = true;
                continue;
            }
            if (quote == Quote::None && std::isspace(static_cast<unsigned char>(character)) != 0) {
                finish_word();
                continue;
            }
            if (quote == Quote::None && (character == '|' || character == '>')) {
                finish_word();
                if (character == '|') {
                    tokens.push_back({
                      .type = TokenType::Pipe,
                      .text = {},
                      .wildcard_mask = {},
                    });
                }
                else if (index + 1U < line.size() && line[index + 1U] == '>') {
                    tokens.push_back({
                      .type = TokenType::Append,
                      .text = {},
                      .wildcard_mask = {},
                    });
                    ++index;
                }
                else {
                    tokens.push_back({
                      .type = TokenType::Redirect,
                      .text = {},
                      .wildcard_mask = {},
                    });
                }
                continue;
            }
            if (quote == Quote::None && (character == '\'' || character == '"')) {
                quote = character == '\'' ? Quote::Single : Quote::Double;
                token_started = true;
                continue;
            }
            if ((quote == Quote::Single && character == '\'') ||
                (quote == Quote::Double && character == '"')) {
                quote = Quote::None;
                continue;
            }

            token.push_back(character);
            wildcard_mask.push_back(quote == Quote::None && character == '*' ? std::uint8_t{ 1U }
                                                                             : std::uint8_t{ 0U });
            token_started = true;
        }

        if (escaped) {
            return std::unexpected("trailing escape character");
        }
        if (quote != Quote::None) {
            return std::unexpected("unterminated quoted string");
        }
        finish_word();
        return tokens;
    }

    [[nodiscard]] auto parse_line(const std::vector<Token>& tokens) -> std::expected<ParsedLine, std::string>
    {
        ParsedLine result;
        result.stages.emplace_back();

        for (std::size_t index{}; index < tokens.size(); ++index) {
            const Token& token{ tokens[index] };
            if (token.type == TokenType::Word) {
                result.stages.back().push_back({
                  .text = token.text,
                  .wildcard_mask = token.wildcard_mask,
                });
                continue;
            }

            if (token.type == TokenType::Pipe) {
                if (result.stages.back().empty()) {
                    return std::unexpected(index == 0U ? "pipeline cannot start with '|'"
                                                       : "pipeline contains an empty command");
                }
                if (result.redirection) {
                    return std::unexpected("output redirection is only allowed after the final command");
                }
                result.stages.emplace_back();
                continue;
            }

            if (result.stages.back().empty()) {
                return std::unexpected("output redirection requires a command");
            }
            if (result.redirection) {
                return std::unexpected("multiple output redirections are not supported");
            }
            if (index + 1U >= tokens.size() || tokens[index + 1U].type != TokenType::Word) {
                return std::unexpected(token.type == TokenType::Append ? "missing file after '>>'"
                                                                       : "missing file after '>'");
            }

            result.redirection = Redirection{
                .target =
                  {
                    .text = tokens[++index].text,
                    .wildcard_mask = tokens[index].wildcard_mask,
                  },
                .append = token.type == TokenType::Append,
            };
            if (index + 1U != tokens.size()) {
                return std::unexpected("output redirection must be at the end of the command line");
            }
        }

        if (!tokens.empty() && result.stages.back().empty()) {
            return std::unexpected("pipeline cannot end with '|'");
        }
        return result;
    }
}

namespace cli
{
    Arguments::Arguments(std::span<const ArgumentSpec> specifications,
                         std::span<const std::string> values,
                         std::span<const FlagSpec> flag_specifications,
                         std::span<const ParsedFlag> flags) noexcept
      : m_specifications{ specifications }
      , m_values{ values }
      , m_flag_specifications{ flag_specifications }
      , m_flags{ flags }
    {
    }

    auto Arguments::size() const noexcept -> std::size_t { return m_values.size(); }

    auto Arguments::empty() const noexcept -> bool { return m_values.empty(); }

    auto Arguments::at(std::size_t index) const -> std::string_view { return m_values.at(index); }

    auto Arguments::get(std::string_view name) const noexcept -> std::optional<std::string_view>
    {
        const auto specification{ std::ranges::find(m_specifications, name, &ArgumentSpec::name) };
        if (specification == m_specifications.end()) {
            return std::nullopt;
        }

        const auto index{ static_cast<std::size_t>(specification - m_specifications.begin()) };
        std::size_t value_index{ index };
        const auto variadic{ std::ranges::find(m_specifications, true, &ArgumentSpec::variadic) };
        if (variadic != m_specifications.end() &&
            static_cast<std::size_t>(variadic - m_specifications.begin()) < index) {
            const std::size_t trailing_specifications{ m_specifications.size() - index };
            if (m_values.size() < trailing_specifications) {
                return std::nullopt;
            }
            value_index = m_values.size() - trailing_specifications;
        }
        return value_index < m_values.size() ? std::optional<std::string_view>{ m_values[value_index] }
                                             : std::nullopt;
    }

    auto Arguments::require(std::string_view name) const -> std::string_view
    {
        const auto value{ get(name) };
        if (!value) {
            throw std::out_of_range{ "CLI argument is absent: " + std::string{ name } };
        }
        return *value;
    }

    auto Arguments::hasFlag(std::string_view name) const noexcept -> bool
    {
        const auto specification{ std::ranges::find(m_flag_specifications, name, &FlagSpec::name) };
        if (specification == m_flag_specifications.end()) {
            return false;
        }

        const auto index{ static_cast<std::size_t>(specification - m_flag_specifications.begin()) };
        return std::ranges::any_of(m_flags,
                                   [index](const auto& flag) { return flag.specification == index; });
    }

    auto Arguments::flagCount(std::string_view name) const noexcept -> std::size_t
    {
        const auto specification{ std::ranges::find(m_flag_specifications, name, &FlagSpec::name) };
        if (specification == m_flag_specifications.end()) {
            return 0U;
        }

        const auto index{ static_cast<std::size_t>(specification - m_flag_specifications.begin()) };
        return static_cast<std::size_t>(std::ranges::count(m_flags, index, &ParsedFlag::specification));
    }

    auto Arguments::flagValue(std::string_view name) const noexcept -> std::optional<std::string_view>
    {
        const auto specification{ std::ranges::find(m_flag_specifications, name, &FlagSpec::name) };
        if (specification == m_flag_specifications.end()) {
            return std::nullopt;
        }

        const auto index{ static_cast<std::size_t>(specification - m_flag_specifications.begin()) };
        const auto flag{ std::ranges::find_last_if(
          m_flags, [index](const auto& candidate) { return candidate.specification == index; }) };
        if (flag.empty() || !flag.front().value) {
            return std::nullopt;
        }
        return *flag.front().value;
    }

    auto Parser::registerCommand(Command command) -> std::expected<void, std::string>
    {
        if (!valid_command_name(command.name)) {
            return std::unexpected("command path must contain valid names separated by single spaces");
        }
        if (command.name == "help" || command.name.starts_with("help ")) {
            return std::unexpected("'help' is reserved by the CLI");
        }
        if (find(command.name) != nullptr) {
            return std::unexpected("command is already registered: " + command.name);
        }
        if (!command.callback) {
            return std::unexpected("command callback is missing: " + command.name);
        }

        bool optional_seen{};
        bool variadic_seen{};
        for (std::size_t index{}; index < command.arguments.size(); ++index) {
            const auto& argument{ command.arguments[index] };
            if (!valid_name(argument.name)) {
                return std::unexpected("argument name must contain only letters, digits, '_', '-' or '.'");
            }
            if (std::ranges::count(command.arguments, argument.name, &ArgumentSpec::name) != 1) {
                return std::unexpected("duplicate argument name: " + argument.name);
            }
            if (!argument.optional && optional_seen) {
                return std::unexpected("required argument follows an optional argument: " + argument.name);
            }
            if (argument.variadic && variadic_seen) {
                return std::unexpected("command has more than one variadic argument");
            }
            if (variadic_seen && argument.optional) {
                return std::unexpected("optional argument follows a variadic argument: " + argument.name);
            }
            optional_seen = optional_seen || argument.optional;
            variadic_seen = variadic_seen || argument.variadic;
        }

        for (const auto& flag : command.flags) {
            if (!valid_name(flag.name)) {
                return std::unexpected("flag name must contain only letters, digits, '_', '-' or '.'");
            }
            if (std::ranges::count(command.flags, flag.name, &FlagSpec::name) != 1) {
                return std::unexpected("duplicate flag name: " + flag.name);
            }
            if (flag.short_name) {
                if (std::isalnum(static_cast<unsigned char>(*flag.short_name)) == 0) {
                    return std::unexpected("short flag name must be a letter or digit: " +
                                           std::string{ *flag.short_name });
                }
                if (std::ranges::count(command.flags, flag.short_name, &FlagSpec::short_name) != 1) {
                    return std::unexpected("duplicate short flag name: -" + std::string{ *flag.short_name });
                }
            }
            if (flag.value_name && !valid_name(*flag.value_name)) {
                return std::unexpected("flag value name is invalid: " + *flag.value_name);
            }
        }

        m_commands.push_back(std::move(command));
        return {};
    }

    auto Parser::find(std::string_view name) const noexcept -> const Command*
    {
        const auto command{ std::ranges::find(m_commands, name, &Command::name) };
        return command == m_commands.end() ? nullptr : &*command;
    }

    auto Parser::contains(std::string_view command_name) const noexcept -> bool
    {
        return command_name == "help" || find(command_name) != nullptr;
    }

    auto Parser::usage(std::string_view command_name) const -> std::string
    {
        const Command* const command{ find(command_name) };
        if (command == nullptr) {
            return {};
        }

        std::string result{ command->name };
        for (const auto& flag : command->flags) {
            result += " [";
            if (flag.short_name) {
                result += '-';
                result += *flag.short_name;
                result += '|';
            }
            result += "--";
            result += flag.name;
            if (flag.value_name) {
                result += " <";
                result += *flag.value_name;
                result += '>';
            }
            result += ']';
        }
        for (const auto& argument : command->arguments) {
            result += argument.optional ? " [" : " <";
            result += argument.name;
            if (argument.variadic) {
                result += "...";
            }
            result += argument.optional ? ']' : '>';
        }
        return result;
    }

    auto Parser::help(std::optional<std::string_view> command_name) const -> std::string
    {
        if (command_name) {
            if (*command_name == "help") {
                return "Usage: help [command]\nShow all commands or detailed help for one command.\n";
            }

            const Command* const command{ find(*command_name) };
            if (command == nullptr) {
                return "unknown command '" + std::string{ *command_name } + "'\n";
            }

            std::ostringstream output;
            output << "Usage: " << usage(command->name) << '\n';
            if (!command->description.empty()) {
                output << command->description << '\n';
            }
            if (!command->arguments.empty()) {
                output << "Arguments:\n";
                for (const auto& argument : command->arguments) {
                    output << "  " << argument.name << (argument.optional ? " (optional)" : " (required)");
                    if (argument.variadic) {
                        output << " (repeatable)";
                    }
                    if (!argument.description.empty()) {
                        output << "  " << argument.description;
                    }
                    output << '\n';
                }
            }
            if (!command->flags.empty()) {
                output << "Flags:\n";
                for (const auto& flag : command->flags) {
                    output << "  ";
                    if (flag.short_name) {
                        output << '-' << *flag.short_name << ", ";
                    }
                    else {
                        output << "    ";
                    }
                    output << "--" << flag.name;
                    if (flag.value_name) {
                        output << " <" << *flag.value_name << '>';
                    }
                    if (!flag.description.empty()) {
                        output << "  " << flag.description;
                    }
                    output << '\n';
                }
            }
            return std::move(output).str();
        }

        std::ostringstream output;
        output << "Available commands:\n"
               << "  help [command]  Show command help\n";
        for (const auto& command : m_commands) {
            output << "  " << usage(command.name);
            if (!command.description.empty()) {
                output << "  " << command.description;
            }
            output << '\n';
        }
        return std::move(output).str();
    }

    auto Parser::execute(std::string_view line, std::ostream& output) const -> ExecutionResult
    {
        const auto tokens{ tokenize(line) };
        if (!tokens) {
            return {
                .error = ExecutionError::InvalidSyntax,
                .message = tokens.error(),
            };
        }
        if (tokens->empty()) {
            return {
                .error = ExecutionError::EmptyInput,
                .exit_code = 0,
                .message = {},
            };
        }

        const auto parsed_line{ parse_line(*tokens) };
        if (!parsed_line) {
            return {
                .error = ExecutionError::InvalidSyntax,
                .message = parsed_line.error(),
            };
        }

        const auto execute_stage = [&](const std::vector<Word>& source_stage,
                                       CommandIO& io) -> ExecutionResult {
            std::vector<std::string> stage;
            stage.reserve(source_stage.size());
            stage.push_back(source_stage.front().text);
            for (std::size_t index{ 1U }; index < source_stage.size(); ++index) {
                const Word& word{ source_stage[index] };
                const auto expanded{ filesystem::expandWildcards(word.text, word.wildcard_mask) };
                if (!expanded) {
                    return {
                        .error = ExecutionError::CallbackFailed,
                        .exit_code = expanded.error().exit_code,
                        .message = "wildcard expansion failed: " + expanded.error().message,
                    };
                }
                stage.insert(stage.end(), expanded->begin(), expanded->end());
            }

            std::string command_name{ stage.front() };
            if (command_name == "help") {
                std::string path;
                for (std::size_t i{ 1U }; i < stage.size(); ++i) {
                    if (!path.empty())
                        path += ' ';
                    path += stage[i];
                }
                const std::optional<std::string_view> requested{
                    path.empty() ? std::nullopt : std::optional<std::string_view>{ path }
                };
                if (requested && *requested != "help" && find(*requested) == nullptr) {
                    return {
                        .error = ExecutionError::UnknownCommand,
                        .message = "unknown command '" + std::string{ *requested } + "'",
                    };
                }
                io.output << help(requested);
                return {};
            }

            // Resolve the longest registered command path before parsing its
            // arguments. Existing one-word commands keep their semantics.
            const Command* command{ find(command_name) };
            std::size_t argument_begin{ 1U };
            std::string candidate{ command_name };
            for (std::size_t i{ 1U }; i < stage.size(); ++i) {
                candidate += ' ';
                candidate += stage[i];
                if (const auto* match{ find(candidate) }) {
                    command = match;
                    command_name = candidate;
                    argument_begin = i + 1U;
                }
            }
            if (command == nullptr) {
                return {
                    .error = ExecutionError::UnknownCommand,
                    .message = "unknown command '" + command_name + "'",
                };
            }

            std::vector<std::string> values;
            std::vector<Arguments::ParsedFlag> parsed_flags;
            bool parse_flags{ true };

            const auto invalid_flag = [&](std::string message) {
                return ExecutionResult{
                    .error = ExecutionError::InvalidArguments,
                    .message = std::move(message) + "; usage: " + usage(command->name),
                };
            };
            const auto long_flag = [&](std::string_view name) {
                return std::ranges::find(command->flags, name, &FlagSpec::name);
            };
            const auto short_flag = [&](char name) {
                return std::ranges::find_if(command->flags,
                                            [name](const auto& flag) { return flag.short_name == name; });
            };
            const auto flag_index = [&](auto iterator) {
                return static_cast<std::size_t>(iterator - command->flags.begin());
            };

            for (std::size_t index{ argument_begin }; index < stage.size(); ++index) {
                const std::string& token{ stage[index] };
                if (parse_flags && token == "--") {
                    parse_flags = false;
                    continue;
                }

                if (parse_flags && token.starts_with("--") && token.size() > 2U) {
                    const std::string_view option{ token };
                    const std::size_t separator{ option.find('=', 2U) };
                    const std::string_view name{ option.substr(
                      2U, separator == std::string_view::npos ? std::string_view::npos : separator - 2U) };
                    const auto specification{ long_flag(name) };
                    if (specification == command->flags.end()) {
                        return invalid_flag("unknown flag '--" + std::string{ name } + "'");
                    }

                    std::optional<std::string> value;
                    if (specification->value_name) {
                        if (separator != std::string_view::npos) {
                            value = option.substr(separator + 1U);
                        }
                        else if (++index < stage.size()) {
                            value = stage[index];
                        }
                        else {
                            return invalid_flag("flag '--" + specification->name + "' requires <" +
                                                *specification->value_name + ">");
                        }
                    }
                    else if (separator != std::string_view::npos) {
                        return invalid_flag("flag '--" + specification->name + "' does not accept a value");
                    }

                    parsed_flags.push_back({
                      .specification = flag_index(specification),
                      .value = std::move(value),
                    });
                    continue;
                }

                if (parse_flags && token.starts_with('-') && token.size() > 1U &&
                    (short_flag(token[1]) != command->flags.end() || !negative_number(token))) {
                    for (std::size_t offset{ 1U }; offset < token.size(); ++offset) {
                        const char name{ token[offset] };
                        const auto specification{ short_flag(name) };
                        if (specification == command->flags.end()) {
                            return invalid_flag("unknown flag '-" + std::string{ name } + "'");
                        }

                        std::optional<std::string> value;
                        if (specification->value_name) {
                            if (offset + 1U < token.size()) {
                                value = token.substr(offset + 1U);
                            }
                            else if (++index < stage.size()) {
                                value = stage[index];
                            }
                            else {
                                return invalid_flag("flag '-" + std::string{ name } + "' requires <" +
                                                    *specification->value_name + ">");
                            }
                        }

                        parsed_flags.push_back({
                          .specification = flag_index(specification),
                          .value = std::move(value),
                        });
                        if (specification->value_name) {
                            break;
                        }
                    }
                    continue;
                }

                values.push_back(token);
            }

            const std::size_t provided{ values.size() };
            const std::size_t required{ static_cast<std::size_t>(
              std::ranges::count(command->arguments, false, &ArgumentSpec::optional)) };
            const bool variadic{ std::ranges::any_of(command->arguments, &ArgumentSpec::variadic) };
            if (provided < required || (!variadic && provided > command->arguments.size())) {
                return {
                    .error = ExecutionError::InvalidArguments,
                    .message = "usage: " + usage(command->name),
                };
            }

            const Arguments arguments{
                command->arguments,
                values,
                command->flags,
                parsed_flags,
            };
            try {
                CallbackResult callback_result{ command->callback(arguments, io) };
                if (!callback_result) {
                    std::string message{ "command '" + command->name + "' failed" };
                    if (!callback_result.error().message.empty()) {
                        message += ": ";
                        message += callback_result.error().message;
                    }
                    return {
                        .error = ExecutionError::CallbackFailed,
                        .exit_code =
                          callback_result.error().exit_code == 0 ? 1 : callback_result.error().exit_code,
                        .message = std::move(message),
                    };
                }
                return {
                    .exit_code = *callback_result,
                    .message = {},
                };
            } catch (const std::exception& exception) {
                return {
                    .error = ExecutionError::CallbackFailed,
                    .exit_code = 1,
                    .message = "command '" + command->name + "' failed: " + exception.what(),
                };
            } catch (...) {
                return {
                    .error = ExecutionError::CallbackFailed,
                    .exit_code = 1,
                    .message = "command '" + command->name + "' failed with an unknown exception",
                };
            }
        };

        std::string pipeline_input;
        for (std::size_t index{}; index < parsed_line->stages.size(); ++index) {
            std::istringstream input{ std::move(pipeline_input) };
            const bool final_stage{ index + 1U == parsed_line->stages.size() };

            if (!final_stage) {
                LimitedStringBuffer buffer;
                std::ostream stage_output{ &buffer };
                CommandIO io{ .input = input, .output = stage_output };
                const ExecutionResult result{ execute_stage(parsed_line->stages[index], io) };
                if (!result) {
                    return result;
                }
                if (buffer.exceeded()) {
                    return {
                        .error = ExecutionError::CallbackFailed,
                        .exit_code = 1,
                        .message = "pipeline output exceeded " + std::to_string(pipeline_capacity) + " bytes",
                    };
                }
                pipeline_input = buffer.take();
                continue;
            }

            if (!parsed_line->redirection) {
                CommandIO io{ .input = input, .output = output };
                return execute_stage(parsed_line->stages[index], io);
            }

            const Word& redirection_word{ parsed_line->redirection->target };
            const auto redirection_targets{ filesystem::expandWildcards(redirection_word.text,
                                                                        redirection_word.wildcard_mask) };
            if (!redirection_targets) {
                return {
                    .error = ExecutionError::CallbackFailed,
                    .exit_code = redirection_targets.error().exit_code,
                    .message =
                      "redirection wildcard expansion failed: " + redirection_targets.error().message,
                };
            }
            if (redirection_targets->size() != 1U) {
                return {
                    .error = ExecutionError::InvalidArguments,
                    .exit_code = 2,
                    .message = "ambiguous output redirection '" + redirection_word.text + "'",
                };
            }

            const auto path{ filesystem::resolvePath(redirection_targets->front(), "output file") };
            if (!path) {
                return {
                    .error = ExecutionError::CallbackFailed,
                    .exit_code = path.error().exit_code,
                    .message = "redirection failed: " + path.error().message,
                };
            }

            errno = 0;
            const auto mode{ std::ios::out | std::ios::binary |
                             (parsed_line->redirection->append ? std::ios::app : std::ios::trunc) };
            std::ofstream file{ *path, mode };
            if (!file.is_open()) {
                const std::error_code error{ errno == 0 ? EIO : errno, std::generic_category() };
                return {
                    .error = ExecutionError::CallbackFailed,
                    .exit_code = 1,
                    .message =
                      "cannot open redirection target '" + path->generic_string() + "': " + error.message(),
                };
            }

            CommandIO io{ .input = input, .output = file };
            const ExecutionResult result{ execute_stage(parsed_line->stages[index], io) };
            file.close();
            if (result.error != ExecutionError::None) {
                return result;
            }
            if (!file) {
                const std::error_code error{ errno == 0 ? EIO : errno, std::generic_category() };
                return {
                    .error = ExecutionError::CallbackFailed,
                    .exit_code = 1,
                    .message =
                      "cannot finish redirection target '" + path->generic_string() + "': " + error.message(),
                };
            }
            return result;
        }

        return {
            .error = ExecutionError::EmptyInput,
            .exit_code = 0,
            .message = {},
        };
    }

    auto Parser::run(std::istream& input, std::ostream& output, std::string_view prompt) const -> void
    {
        std::string line;
        while (output << prompt << std::flush, std::getline(input, line)) {
            const ExecutionResult result{ execute(line, output) };
            if (result.error != ExecutionError::None && result.error != ExecutionError::EmptyInput) {
                output << "error: " << result.message << '\n';
            }
        }
    }

    auto registry() noexcept -> Parser&
    {
        static Parser parser;
        return parser;
    }
}
