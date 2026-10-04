#pragma once

#include "pneumo/meta.hpp"

#include <concepts>
#include <expected>
#include <functional>
#include <iosfwd>
#include <meta>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cli
{
    struct ArgumentSpec
    {
        std::string name;
        std::string description;
        bool optional{};
        bool variadic{};
    };

    struct FlagSpec
    {
        std::string name;
        std::optional<char> short_name;
        std::string description;
        std::optional<std::string> value_name;
    };

    class Arguments
    {
      public:
        [[nodiscard]] auto size() const noexcept -> std::size_t;
        [[nodiscard]] auto empty() const noexcept -> bool;
        [[nodiscard]] auto at(std::size_t index) const -> std::string_view;
        [[nodiscard]] auto get(std::string_view name) const noexcept -> std::optional<std::string_view>;
        [[nodiscard]] auto require(std::string_view name) const -> std::string_view;
        [[nodiscard]] auto hasFlag(std::string_view name) const noexcept -> bool;
        [[nodiscard]] auto flagCount(std::string_view name) const noexcept -> std::size_t;
        [[nodiscard]] auto flagValue(std::string_view name) const noexcept -> std::optional<std::string_view>;

      private:
        friend class Parser;

        struct ParsedFlag
        {
            std::size_t specification{};
            std::optional<std::string> value;
        };

        Arguments(std::span<const ArgumentSpec> specifications,
                  std::span<const std::string> values,
                  std::span<const FlagSpec> flag_specifications,
                  std::span<const ParsedFlag> flags) noexcept;

        std::span<const ArgumentSpec> m_specifications;
        std::span<const std::string> m_values;
        std::span<const FlagSpec> m_flag_specifications;
        std::span<const ParsedFlag> m_flags;
    };

    struct CallbackError
    {
        std::string message;
        int exit_code{ 1 };
    };

    using CallbackResult = std::expected<int, CallbackError>;

    struct CommandIO
    {
        std::istream& input;
        std::ostream& output;
    };

    /**
     * Type-erased command callback.
     *
     * Stream-aware callbacks receive CommandIO. The legacy ostream signature
     * remains accepted so existing commands do not need to change.
     */
    class Callback
    {
      public:
        Callback() = default;

        template<typename Function>
            requires std::invocable<std::decay_t<Function>&, const Arguments&, CommandIO&> &&
                     std::convertible_to<
                       std::invoke_result_t<std::decay_t<Function>&, const Arguments&, CommandIO&>,
                       CallbackResult>
        Callback(Function&& function)
          : m_callback{ [callback = std::forward<Function>(
                           function)](const Arguments& arguments, CommandIO& io) mutable -> CallbackResult {
              return std::invoke(callback, arguments, io);
          } }
        {
        }

        template<typename Function>
            requires(!std::invocable<std::decay_t<Function>&, const Arguments&, CommandIO&>) &&
                    std::invocable<std::decay_t<Function>&, const Arguments&, std::ostream&> &&
                    std::convertible_to<
                      std::invoke_result_t<std::decay_t<Function>&, const Arguments&, std::ostream&>,
                      CallbackResult>
        Callback(Function&& function)
          : m_callback{ [callback = std::forward<Function>(
                           function)](const Arguments& arguments, CommandIO& io) mutable -> CallbackResult {
              return std::invoke(callback, arguments, io.output);
          } }
        {
        }

        [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(m_callback); }

        auto operator()(const Arguments& arguments, CommandIO& io) const -> CallbackResult
        {
            return m_callback(arguments, io);
        }

      private:
        std::function<CallbackResult(const Arguments&, CommandIO&)> m_callback;
    };

    /**
     * Return a diagnostic from a command without throwing or writing it directly
     * to the terminal. The parser adds the command name and reports it exactly
     * like a contained callback exception.
     */
    [[nodiscard]] inline auto callback_failure(std::string message, int exit_code = 1) -> CallbackResult
    {
        return std::unexpected(CallbackError{
          .message = std::move(message),
          .exit_code = exit_code == 0 ? 1 : exit_code,
        });
    }

    struct Command
    {
        // One or more name components, e.g. "motor move" or "robot status".
        std::string name;
        std::string description;
        std::vector<ArgumentSpec> arguments;
        std::vector<FlagSpec> flags;
        Callback callback;
    };

    enum class ExecutionError
    {
        None,
        EmptyInput,
        InvalidSyntax,
        UnknownCommand,
        InvalidArguments,
        CallbackFailed,
    };

    struct ExecutionResult
    {
        ExecutionError error{ ExecutionError::None };
        int exit_code{};
        std::string message;

        [[nodiscard]] explicit operator bool() const noexcept { return error == ExecutionError::None; }
    };

    class Parser
    {
      public:
        [[nodiscard]] auto registerCommand(Command command) -> std::expected<void, std::string>;

        [[nodiscard]] auto contains(std::string_view command_name) const noexcept -> bool;

        [[nodiscard]] auto execute(std::string_view line, std::ostream& output) const -> ExecutionResult;

        auto run(std::istream& input, std::ostream& output, std::string_view prompt = "> ") const -> void;

        [[nodiscard]] auto usage(std::string_view command_name) const -> std::string;
        [[nodiscard]] auto help(std::optional<std::string_view> command_name = std::nullopt) const
          -> std::string;

      private:
        [[nodiscard]] auto find(std::string_view name) const noexcept -> const Command*;

        std::vector<Command> m_commands;
    };

    /**
     * Parser consumed by the application's CLI thread.
     *
     * Register application commands before system_threads::start() launches
     * that thread. Parser instances created directly are otherwise independent.
     */
    [[nodiscard]] auto registry() noexcept -> Parser&;

    template<size_t N1, size_t N2>
    struct Arg
    {
        pnm::meta::string::FixedString<N1> name{ "" };
        pnm::meta::string::FixedString<N2> description{ "" };
        bool optional{ false };
        bool variadic{ false };
    };

    // Optional command path override, e.g. "motor status". Without this
    // annotation the reflected function identifier is the command name.
    template<size_t N>
    struct Name
    {
        pnm::meta::string::FixedString<N> name;
    };

    template<size_t N1, size_t N2, size_t N3 = 0UZ>
    struct Flag
    {
        pnm::meta::string::FixedString<N1> name{ "" };
        char short_name{ '\0' };
        pnm::meta::string::FixedString<N2> description{ "" };
        pnm::meta::string::FixedString<N3> value_name{ "" };
    };

    namespace detail
    {
        template<typename T>
        struct is_name : std::false_type
        {
        };

        template<size_t N>
        struct is_name<Name<N>> : std::true_type
        {
        };

        template<typename T>
        concept IsName = is_name<std::remove_cvref_t<T>>::value;

        template<typename T>
        struct is_arg : std::false_type
        {
        };

        template<size_t... Ns>
        struct is_arg<Arg<Ns...>> : std::true_type
        {
        };

        template<typename T>
        inline constexpr bool is_arg_v = is_arg<std::remove_cvref_t<T>>::value;

        template<typename T>
        concept IsArg = is_arg_v<T>;

        template<typename T>
        struct is_flag : std::false_type
        {
        };

        template<size_t... Ns>
        struct is_flag<Flag<Ns...>> : std::true_type
        {
        };

        template<typename T>
        inline constexpr bool is_flag_v = is_flag<std::remove_cvref_t<T>>::value;

        template<typename T>
        concept IsFlag = is_flag_v<T>;
    }

    // Bind adapts a reflected function to Callback (for example by capturing
    // an application-owned controller). Stateless modules use the identity
    // binding and the application registry, as before.
    template<std::meta::info ns = std::meta::current_namespace(), typename Bind = std::identity>
    constexpr auto register_commands(Parser& parser = registry(), Bind bind = {}) -> void
    {
        static constexpr auto command_functions{ [] {
            std::vector<std::meta::info> functions;
            for (auto func :
                 std::meta::members_of(^^[:ns:] ::commands, std::meta::access_context::current())) {
                if (std::meta::is_function(func)) {
                    functions.push_back(func);
                }
            }
            return std::define_static_array(functions);
        }() };

        template for (constexpr auto command_function : command_functions)
        {
            constexpr auto annotations{ std::define_static_array(
              std::meta::annotations_of(command_function)) };
            static_assert(!annotations.empty(), "Command function must have at least one annotation.");

            using DescriptionType = std::remove_cvref_t<typename[:std::meta::type_of(annotations[0]):]>;
            static_assert(pnm::meta::string::FixedStringLike<DescriptionType>,
                          "The first annotation must be a fixed string describing the command!");

            constexpr auto name{ std::meta::identifier_of(command_function) };
            constexpr auto description{ std::meta::extract<DescriptionType>(annotations[0]) };

            Command cmd{ .name = std::string(name),
                         .description = std::string(description),
                         .arguments = {},
                         .flags = {},
                         .callback = bind([:command_function:]) };

            template for (constexpr auto annotation :
                          std::span(annotations.data() + 1, annotations.size() - 1))
            {
                using Type = std::remove_cvref_t<typename[:std::meta::type_of(annotation):]>;
                static_assert(detail::IsName<Type> || detail::IsArg<Type> || detail::IsFlag<Type>,
                              "Invalid type for secondary annotation!");

                if constexpr (detail::IsName<Type>) {
                    constexpr auto name_override{ std::meta::extract<Type>(annotation) };
                    static_assert(!name_override.name.empty(), "Command name must not be empty.");
                    cmd.name = std::string(name_override.name);
                }
                else if constexpr (detail::IsArg<Type>) {
                    constexpr auto arg{ std::meta::extract<Type>(annotation) };
                    cmd.arguments.emplace_back(
                      std::string(arg.name), std::string(arg.description), arg.optional, arg.variadic);
                }
                else if constexpr (detail::IsFlag<Type>) {
                    constexpr auto flag{ std::meta::extract<Type>(annotation) };
                    cmd.flags.emplace_back(
                      std::string(flag.name),
                      flag.short_name == '\0' ? std::nullopt : std::optional<char>{ flag.short_name },
                      std::string(flag.description),
                      flag.value_name.empty() ? std::nullopt : std::optional<std::string>{ flag.value_name });
                }
            }

            auto registration{ parser.registerCommand(std::move(cmd)) };
            if (!registration) {
                throw std::runtime_error{ registration.error() };
            }
        }
    }
}
