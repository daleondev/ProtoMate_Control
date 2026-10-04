#include "cli/Parser.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>

namespace
{
    auto register_copy_command(cli::Parser& parser, std::string& first, std::string& second, int& calls)
      -> void
    {
        auto result{ parser.registerCommand({
          .name = "copy",
          .description = "Copy a source to an optional destination",
          .arguments =
            {
              { .name = "source", .description = "Source path" },
              { .name = "destination", .description = "Destination path", .optional = true },
            },
          .flags = {},
          .callback =
            [&](const cli::Arguments& arguments, std::ostream&) {
                ++calls;
                first = arguments.require("source");
                second = arguments.get("destination").value_or("");
                return 7;
            },
        }) };
        ASSERT_TRUE(result) << result.error();
    }
}

TEST(CliParser, InvokesARegisteredCommandWithRequiredAndOptionalArguments)
{
    cli::Parser parser;
    std::string source;
    std::string destination;
    int calls{};
    register_copy_command(parser, source, destination, calls);

    std::ostringstream output;
    const auto first{ parser.execute("copy alpha", output) };
    EXPECT_TRUE(first);
    EXPECT_EQ(first.exit_code, 7);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(source, "alpha");
    EXPECT_TRUE(destination.empty());

    const auto second{ parser.execute("copy alpha beta", output) };
    EXPECT_TRUE(second);
    EXPECT_EQ(calls, 2);
    EXPECT_EQ(source, "alpha");
    EXPECT_EQ(destination, "beta");
}

TEST(CliParser, MapsRequiredArgumentsAfterAVariadicArgument)
{
    cli::Parser parser;
    std::string first_source;
    std::string destination;
    std::size_t value_count{};
    ASSERT_TRUE(parser.registerCommand({
      .name = "multi",
      .description = {},
      .arguments =
        {
          { .name = "source", .description = {}, .variadic = true },
          { .name = "destination", .description = {} },
        },
      .flags = {},
      .callback =
        [&](const cli::Arguments& arguments, std::ostream&) {
            first_source = arguments.require("source");
            destination = arguments.require("destination");
            value_count = arguments.size();
            return 0;
        },
    }));

    std::ostringstream output;
    const auto result{ parser.execute("multi first second destination", output) };
    EXPECT_TRUE(result) << result.message;
    EXPECT_EQ(first_source, "first");
    EXPECT_EQ(destination, "destination");
    EXPECT_EQ(value_count, 3U);
    EXPECT_EQ(parser.execute("multi only-source", output).error, cli::ExecutionError::InvalidArguments);
}

TEST(CliParser, TokenizesQuotesEscapesAndEmptyArguments)
{
    cli::Parser parser;
    std::string source;
    std::string destination;
    int calls{};
    register_copy_command(parser, source, destination, calls);
    std::ostringstream output;

    EXPECT_TRUE(parser.execute(R"(copy "source file" destination\ path)", output));
    EXPECT_EQ(source, "source file");
    EXPECT_EQ(destination, "destination path");

    EXPECT_TRUE(parser.execute(R"(copy '' "")", output));
    EXPECT_TRUE(source.empty());
    EXPECT_TRUE(destination.empty());
}

TEST(CliParser, ParsesBundledLongAndValueFlags)
{
    cli::Parser parser;
    std::size_t all_count{};
    bool long_listing{};
    std::string format;
    std::string path;
    int calls{};
    ASSERT_TRUE(parser.registerCommand({
      .name = "list",
      .description = "List a path",
      .arguments =
        {
          { .name = "path", .description = "Path to list", .optional = true },
        },
      .flags =
        {
          {
            .name = "all",
            .short_name = 'a',
            .description = "Include hidden entries",
            .value_name = std::nullopt,
          },
          {
            .name = "long",
            .short_name = 'l',
            .description = "Use the long format",
            .value_name = std::nullopt,
          },
          {
            .name = "format",
            .short_name = 'f',
            .description = "Select an output format",
            .value_name = "name",
          },
        },
      .callback =
        [&](const cli::Arguments& arguments, std::ostream&) {
            ++calls;
            all_count = arguments.flagCount("all");
            long_listing = arguments.hasFlag("long");
            format = arguments.flagValue("format").value_or("");
            path = arguments.get("path").value_or("");
            return 0;
        },
    }));

    std::ostringstream output;
    EXPECT_TRUE(parser.execute("list -aal -fwide /tmp", output));
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(all_count, 2U);
    EXPECT_TRUE(long_listing);
    EXPECT_EQ(format, "wide");
    EXPECT_EQ(path, "/tmp");

    EXPECT_TRUE(parser.execute("list --long --format=single -- -leading-dash", output));
    EXPECT_EQ(calls, 2);
    EXPECT_EQ(all_count, 0U);
    EXPECT_TRUE(long_listing);
    EXPECT_EQ(format, "single");
    EXPECT_EQ(path, "-leading-dash");

    EXPECT_TRUE(parser.execute("list /tmp --format records", output));
    EXPECT_EQ(format, "records");
}

TEST(CliParser, ConnectsCommandStreamsWithPipes)
{
    cli::Parser parser;
    ASSERT_TRUE(parser.registerCommand({
      .name = "source",
      .description = {},
      .arguments = {},
      .flags = {},
      .callback =
        [](const cli::Arguments&, std::ostream& output) {
        output << "first\nsecond value\nthird\n";
        return 0;
    },
    }));
    ASSERT_TRUE(parser.registerCommand({
      .name = "select",
      .description = {},
      .arguments = { { .name = "text", .description = {} } },
      .flags = {},
      .callback =
        [](const cli::Arguments& arguments, cli::CommandIO& io) {
        std::string line;
        while (std::getline(io.input, line)) {
            if (line.find(arguments.require("text")) != std::string::npos) {
                io.output << line << '\n';
            }
        }
        return 0;
    },
    }));

    std::ostringstream output;
    const auto result{ parser.execute(R"(source|select "second value"|select value)", output) };
    EXPECT_TRUE(result) << result.message;
    EXPECT_EQ(output.str(), "second value\n");
}

TEST(CliParser, TreatsQuotedAndEscapedOperatorsAsArguments)
{
    cli::Parser parser;
    ASSERT_TRUE(parser.registerCommand({
      .name = "join",
      .description = {},
      .arguments =
        {
          {
            .name = "values",
            .description = {},
            .optional = true,
            .variadic = true,
          },
        },
      .flags = {},
      .callback =
        [](const cli::Arguments& arguments, std::ostream& output) {
            for (std::size_t index{}; index < arguments.size(); ++index) {
                output << (index == 0U ? "" : ",") << arguments.at(index);
            }
            return 0;
        },
    }));

    std::ostringstream output;
    const auto result{ parser.execute(R"(join "left|right" 'a>b' escaped\>value)", output) };
    EXPECT_TRUE(result) << result.message;
    EXPECT_EQ(output.str(), "left|right,a>b,escaped>value");
    EXPECT_EQ(parser.usage("join"), "join [values...]");
}

TEST(CliParser, ExpandsOnlyUnquotedAsteriskWildcards)
{
    namespace fs = std::filesystem;

    cli::Parser parser;
    ASSERT_TRUE(parser.registerCommand({
      .name = "join",
      .description = {},
      .arguments =
        {
          {
            .name = "values",
            .description = {},
            .optional = true,
            .variadic = true,
          },
        },
      .flags = {},
      .callback =
        [](const cli::Arguments& arguments, std::ostream& output) {
            for (std::size_t index{}; index < arguments.size(); ++index) {
                output << (index == 0U ? "" : "\n") << arguments.at(index);
            }
            return 0;
        },
    }));
    ASSERT_TRUE(parser.registerCommand({
      .name = "write",
      .description = {},
      .arguments = {},
      .flags = {},
      .callback =
        [](const cli::Arguments&, std::ostream& output) {
        output << "data\n";
        return 0;
    },
    }));

    const fs::path directory{ fs::temp_directory_path() /
                              ("simplcity-cli-wildcards-" + std::to_string(::getpid())) };
    std::error_code error;
    static_cast<void>(fs::remove_all(directory, error));
    ASSERT_FALSE(error);
    ASSERT_TRUE(fs::create_directories(directory / "nested"));
    for (const auto& path : {
           directory / "alpha.txt",
           directory / "beta.txt",
           directory / "gamma.bin",
           directory / ".hidden.txt",
           directory / "nested/inside.txt",
         }) {
        std::ofstream file{ path };
        ASSERT_TRUE(file.is_open());
        file << path.filename().generic_string() << '\n';
    }

    const std::string prefix{ directory.generic_string() };
    std::ostringstream output;
    auto result{ parser.execute("join " + prefix + "/*.txt", output) };
    ASSERT_TRUE(result) << result.message;
    EXPECT_EQ(output.str(), prefix + "/alpha.txt\n" + prefix + "/beta.txt");

    output.str({});
    output.clear();
    result = parser.execute("join " + prefix + "/n*/inside.*", output);
    ASSERT_TRUE(result) << result.message;
    EXPECT_EQ(output.str(), prefix + "/nested/inside.txt");

    output.str({});
    output.clear();
    result = parser.execute("join \"" + prefix + "/*.txt\" " + prefix + R"(/\*.txt)", output);
    ASSERT_TRUE(result) << result.message;
    EXPECT_EQ(output.str(), prefix + "/*.txt\n" + prefix + "/*.txt");

    output.str({});
    output.clear();
    result = parser.execute("join " + prefix + "/missing-*.txt", output);
    ASSERT_TRUE(result) << result.message;
    EXPECT_EQ(output.str(), prefix + "/missing-*.txt");

    result = parser.execute("write > " + prefix + "/*.txt", output);
    EXPECT_EQ(result.error, cli::ExecutionError::InvalidArguments);
    EXPECT_NE(result.message.find("ambiguous output redirection"), std::string::npos);

    EXPECT_EQ(fs::remove_all(directory), 7U);
}

TEST(CliParser, RedirectsAndAppendsOutput)
{
    namespace fs = std::filesystem;

    cli::Parser parser;
    ASSERT_TRUE(parser.registerCommand({
      .name = "write",
      .description = {},
      .arguments = { { .name = "text", .description = {} } },
      .flags = {},
      .callback =
        [](const cli::Arguments& arguments, std::ostream& output) {
        output << arguments.require("text") << '\n';
        return 0;
    },
    }));

    const fs::path path{ fs::temp_directory_path() /
                         ("simplcity-cli-redirection-" + std::to_string(::getpid())) };
    std::error_code error;
    static_cast<void>(fs::remove(path, error));

    std::ostringstream output;
    auto result{ parser.execute("write first > \"" + path.generic_string() + '"', output) };
    ASSERT_TRUE(result) << result.message;
    result = parser.execute("write second>>\"" + path.generic_string() + '"', output);
    ASSERT_TRUE(result) << result.message;
    EXPECT_TRUE(output.str().empty());

    std::ifstream file{ path };
    ASSERT_TRUE(file.is_open());
    std::ostringstream contents;
    contents << file.rdbuf();
    EXPECT_EQ(contents.str(), "first\nsecond\n");
    file.close();
    EXPECT_TRUE(fs::remove(path));
}

TEST(CliParser, RejectsMalformedPipelinesAndBoundsIntermediateOutput)
{
    cli::Parser parser;
    int sink_calls{};
    ASSERT_TRUE(parser.registerCommand({
      .name = "flood",
      .description = {},
      .arguments = {},
      .flags = {},
      .callback =
        [](const cli::Arguments&, std::ostream& output) {
        output << std::string(40U * 1024U, 'x');
        return 0;
    },
    }));
    ASSERT_TRUE(parser.registerCommand({
      .name = "sink",
      .description = {},
      .arguments = {},
      .flags = {},
      .callback =
        [&sink_calls](const cli::Arguments&, cli::CommandIO&) {
        ++sink_calls;
        return 0;
    },
    }));

    std::ostringstream output;
    EXPECT_EQ(parser.execute("| flood", output).error, cli::ExecutionError::InvalidSyntax);
    EXPECT_EQ(parser.execute("flood |", output).error, cli::ExecutionError::InvalidSyntax);
    EXPECT_EQ(parser.execute("flood || sink", output).error, cli::ExecutionError::InvalidSyntax);
    EXPECT_EQ(parser.execute("flood >", output).error, cli::ExecutionError::InvalidSyntax);
    EXPECT_EQ(parser.execute("flood > file extra", output).error, cli::ExecutionError::InvalidSyntax);

    const auto overflow{ parser.execute("flood | sink", output) };
    EXPECT_EQ(overflow.error, cli::ExecutionError::CallbackFailed);
    EXPECT_NE(overflow.message.find("pipeline output exceeded"), std::string::npos);
    EXPECT_EQ(sink_calls, 0);
}

TEST(CliParser, RejectsUnknownFlagsAndMissingOrUnexpectedFlagValues)
{
    cli::Parser parser;
    int calls{};
    ASSERT_TRUE(parser.registerCommand({
      .name = "list",
      .description = {},
      .arguments = {},
      .flags =
        {
          {
            .name = "all",
            .short_name = 'a',
            .description = {},
            .value_name = std::nullopt,
          },
          {
            .name = "format",
            .short_name = 'f',
            .description = {},
            .value_name = "name",
          },
        },
      .callback =
        [&](const cli::Arguments&, std::ostream&) {
            ++calls;
            return 0;
        },
    }));

    std::ostringstream output;
    EXPECT_EQ(parser.execute("list -x", output).error, cli::ExecutionError::InvalidArguments);
    EXPECT_EQ(parser.execute("list --missing", output).error, cli::ExecutionError::InvalidArguments);
    EXPECT_EQ(parser.execute("list -f", output).error, cli::ExecutionError::InvalidArguments);
    EXPECT_EQ(parser.execute("list --format", output).error, cli::ExecutionError::InvalidArguments);
    EXPECT_EQ(parser.execute("list --all=true", output).error, cli::ExecutionError::InvalidArguments);
    EXPECT_EQ(calls, 0);
}

TEST(CliParser, RejectsMalformedInputAndWrongArgumentCounts)
{
    cli::Parser parser;
    std::string source;
    std::string destination;
    int calls{};
    register_copy_command(parser, source, destination, calls);
    std::ostringstream output;

    const auto unterminated{ parser.execute(R"(copy "source)", output) };
    EXPECT_EQ(unterminated.error, cli::ExecutionError::InvalidSyntax);

    const auto trailing_escape{ parser.execute("copy source\\", output) };
    EXPECT_EQ(trailing_escape.error, cli::ExecutionError::InvalidSyntax);

    const auto too_few{ parser.execute("copy", output) };
    EXPECT_EQ(too_few.error, cli::ExecutionError::InvalidArguments);
    EXPECT_EQ(too_few.message, "usage: copy <source> [destination]");

    const auto too_many{ parser.execute("copy one two three", output) };
    EXPECT_EQ(too_many.error, cli::ExecutionError::InvalidArguments);
    EXPECT_EQ(calls, 0);
}

TEST(CliParser, RejectsInvalidRegistrations)
{
    cli::Parser parser;
    const auto callback{ [](const cli::Arguments&, std::ostream&) { return 0; } };

    EXPECT_FALSE(parser.registerCommand({
      .name = "bad  name",
      .description = {},
      .arguments = {},
      .flags = {},
      .callback = callback,
    }));
    EXPECT_FALSE(parser.registerCommand({
      .name = "help",
      .description = {},
      .arguments = {},
      .flags = {},
      .callback = callback,
    }));
    EXPECT_FALSE(parser.registerCommand({
      .name = "missing-callback",
      .description = {},
      .arguments = {},
      .flags = {},
      .callback = {},
    }));

    ASSERT_TRUE(parser.registerCommand({
      .name = "valid",
      .description = {},
      .arguments = { { .name = "optional", .description = {}, .optional = true } },
      .flags = {},
      .callback = callback,
    }));
    EXPECT_FALSE(parser.registerCommand({
      .name = "valid",
      .description = {},
      .arguments = {},
      .flags = {},
      .callback = callback,
    }));
    EXPECT_FALSE(parser.registerCommand({
      .name = "bad-order",
      .description = {},
      .arguments =
        {
          { .name = "optional", .description = {}, .optional = true },
          { .name = "required", .description = {} },
        },
      .flags = {},
      .callback = callback,
    }));
    EXPECT_FALSE(parser.registerCommand({
      .name = "multiple-variadic",
      .description = {},
      .arguments =
        {
          { .name = "first", .description = {}, .variadic = true },
          { .name = "second", .description = {}, .variadic = true },
        },
      .flags = {},
      .callback = callback,
    }));
    EXPECT_FALSE(parser.registerCommand({
      .name = "duplicates",
      .description = {},
      .arguments =
        {
          { .name = "value", .description = {} },
          { .name = "value", .description = {} },
        },
      .flags = {},
      .callback = callback,
    }));
    EXPECT_FALSE(parser.registerCommand({
      .name = "duplicate-flags",
      .description = {},
      .arguments = {},
      .flags =
        {
          {
            .name = "all",
            .short_name = 'a',
            .description = {},
            .value_name = std::nullopt,
          },
          {
            .name = "almost-all",
            .short_name = 'a',
            .description = {},
            .value_name = std::nullopt,
          },
        },
      .callback = callback,
    }));
}

TEST(CliParser, ProvidesBuiltInHelpAndReportsUnknownCommands)
{
    cli::Parser parser;
    std::string source;
    std::string destination;
    int calls{};
    register_copy_command(parser, source, destination, calls);

    std::ostringstream output;
    EXPECT_TRUE(parser.execute("help", output));
    EXPECT_NE(output.str().find("copy <source> [destination]"), std::string::npos);

    output.str({});
    output.clear();
    EXPECT_TRUE(parser.execute("help copy", output));
    EXPECT_NE(output.str().find("Usage: copy <source> [destination]"), std::string::npos);
    EXPECT_NE(output.str().find("source (required)"), std::string::npos);

    const auto unknown{ parser.execute("missing", output) };
    EXPECT_EQ(unknown.error, cli::ExecutionError::UnknownCommand);
    EXPECT_EQ(unknown.message, "unknown command 'missing'");
}

TEST(CliParser, ContainsCallbackExceptionsAndKeepsTheTerminalRunning)
{
    cli::Parser parser;
    ASSERT_TRUE(parser.registerCommand({
      .name = "fail",
      .description = {},
      .arguments = {},
      .flags = {},
      .callback = [](const cli::Arguments&, std::ostream&) -> int {
        throw std::runtime_error{ "expected failure" };
    },
    }));

    std::ostringstream output;
    const auto result{ parser.execute("fail", output) };
    EXPECT_EQ(result.error, cli::ExecutionError::CallbackFailed);
    EXPECT_EQ(result.exit_code, 1);
    EXPECT_NE(result.message.find("expected failure"), std::string::npos);
}

TEST(CliParser, ReportsStructuredcallback_failures)
{
    cli::Parser parser;
    ASSERT_TRUE(parser.registerCommand({
      .name = "fail",
      .description = {},
      .arguments = {},
      .flags = {},
      .callback =
        [](const cli::Arguments&, std::ostream&) {
        return cli::callback_failure("the requested operation was rejected", 23);
    },
    }));

    std::ostringstream output;
    const auto result{ parser.execute("fail", output) };
    EXPECT_EQ(result.error, cli::ExecutionError::CallbackFailed);
    EXPECT_EQ(result.exit_code, 23);
    EXPECT_EQ(result.message, "command 'fail' failed: the requested operation was rejected");
}

TEST(CliParser, RunsAnInteractivePromptUntilEndOfInput)
{
    cli::Parser parser;
    ASSERT_TRUE(parser.registerCommand({
      .name = "ping",
      .description = {},
      .arguments = {},
      .flags = {},
      .callback =
        [](const cli::Arguments&, std::ostream& output) {
        output << "pong\n";
        return 0;
    },
    }));

    std::istringstream input{ "ping\nmissing\n\n" };
    std::ostringstream output;
    parser.run(input, output, "$ ");

    EXPECT_EQ(output.str(), "$ pong\n$ error: unknown command 'missing'\n$ $ ");
}

TEST(CliParser, ResolvesLongestCommandPathsWithIndependentHelpAndFlags)
{
    cli::Parser parser;
    ASSERT_TRUE(parser.registerCommand({ "robot", "Robot commands", {}, {},
      [](const cli::Arguments&, std::ostream& out) { out << "group\n"; return 0; } }));
    ASSERT_TRUE(parser.registerCommand({ "robot axis move", "Move one robot axis",
      { { "distance", "Signed distance" } },
      { { "speed", 's', "Speed", "value" } },
      [](const cli::Arguments& args, std::ostream& out) {
          out << args.require("distance") << ' ' << args.flagValue("speed").value_or("unset") << '\n';
          return 0;
      } }));
    std::ostringstream output;
    ASSERT_TRUE(parser.execute("robot axis move -9e-1 --speed 2", output));
    EXPECT_EQ(output.str(), "-9e-1 2\n");
    output.str({});
    ASSERT_TRUE(parser.execute("help robot axis move", output));
    EXPECT_NE(output.str().find("Usage: robot axis move"), std::string::npos);
    EXPECT_NE(output.str().find("Signed distance"), std::string::npos);
    EXPECT_FALSE(parser.execute("robot axis move 1 --unknown", output));
    EXPECT_FALSE(parser.execute("robot unknown", output));
    EXPECT_TRUE(parser.contains("robot axis move"));
}

TEST(CliParser, NegativeNumbersRemainArgumentsUnlessTheNumericShortFlagIsRegistered)
{
    cli::Parser parser;
    ASSERT_TRUE(parser.registerCommand({ "number", {}, { { "value", {} } }, {},
      [](const cli::Arguments& args, std::ostream& out) { out << args.require("value"); return 0; } }));
    ASSERT_TRUE(parser.registerCommand({ "numericflag", {}, {}, { { "one", '1', {}, std::nullopt } },
      [](const cli::Arguments& args, std::ostream& out) { out << args.hasFlag("one"); return 0; } }));
    for (const auto number : { "-90", "-.5", "-1e-3" }) {
        std::ostringstream output;
        ASSERT_TRUE(parser.execute(std::string("number ") + number, output));
        EXPECT_EQ(output.str(), number);
    }
    std::ostringstream output;
    ASSERT_TRUE(parser.execute("numericflag -1", output));
    EXPECT_EQ(output.str(), "1");
    EXPECT_FALSE(parser.execute("number -typo", output));
}

TEST(CliParser, RejectsMalformedCommandPathsAndReservedHelpPaths)
{
    cli::Parser parser;
    for (const auto name : { " motor", "motor ", "motor  move", "motor\tmove", "help motor" })
        EXPECT_FALSE(parser.registerCommand({ name, {}, {}, {},
          [](const cli::Arguments&, std::ostream&) { return 0; } }));
}
