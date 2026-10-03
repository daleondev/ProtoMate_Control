#include "cli/Parser.hpp"
#include "cli/utilities.hpp"

#include <gtest/gtest.h>

#include <sstream>
#include <string>

TEST(CliUtilities, TimeProfilesAQuotedPipelineAndPreservesItsOutput)
{
    cli::utilities::setup();
    std::ostringstream output;
    const auto result{ cli::registry().execute(R"(time "echo 'hello world' | grep hello")", output) };
    ASSERT_TRUE(result) << result.message;
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_TRUE(output.str().starts_with("hello world\nCPU (all threads): "));
    EXPECT_NE(output.str().find(" ms\nElapsed: "), std::string::npos);
}

TEST(CliUtilities, TimePreservesNonzeroCommandExitStatus)
{
    cli::utilities::setup();
    auto& parser{ cli::registry() };
    ASSERT_TRUE(parser.registerCommand({
      .name = "profiling_test_status",
      .description = "test exit status",
      .arguments = {},
      .flags = {},
      .callback = [](const cli::Arguments&, cli::CommandIO&) -> cli::CallbackResult { return 7; },
    }));
    std::ostringstream output;
    const auto result{ parser.execute("time profiling_test_status", output) };
    ASSERT_TRUE(result) << result.message;
    EXPECT_EQ(result.exit_code, 7);
    EXPECT_TRUE(output.str().starts_with("CPU (all threads): "));
}

TEST(CliUtilities, TimeReportsCommandErrorsAndRejectsEmptyCommands)
{
    cli::utilities::setup();
    std::ostringstream output;
    const auto result{ cli::registry().execute("time no_such_profiling_command", output) };
    EXPECT_EQ(result.error, cli::ExecutionError::CallbackFailed);
    EXPECT_NE(result.exit_code, 0);
    EXPECT_NE(result.message.find("no_such_profiling_command"), std::string::npos);
    EXPECT_NE(output.str().find("Elapsed: "), std::string::npos);
    const auto empty{ cli::registry().execute(R"(time "")", output) };
    EXPECT_EQ(empty.error, cli::ExecutionError::CallbackFailed);
    EXPECT_NE(empty.message.find("empty command"), std::string::npos);
}
