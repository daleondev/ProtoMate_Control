#include "cli/Parser.hpp"
#include "cli/filesystem.hpp"
#include "cli/utilities.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

namespace
{
    namespace fs = std::filesystem;

    [[nodiscard]] auto execute(cli::Parser& parser, std::string_view command, std::ostringstream& output)
      -> cli::ExecutionResult
    {
        output.str({});
        output.clear();
        return parser.execute(command, output);
    }
}

TEST(CliFilesystem, SupportsCommonFileAndDirectoryOperations)
{
    auto& parser{ cli::registry() };
    cli::filesystem::setup();
    cli::utilities::setup();
    cli::filesystem::setup();
    cli::utilities::setup();

    std::ostringstream output;
    ASSERT_TRUE(execute(parser, "pwd", output));
    std::string original_directory{ output.str() };
    ASSERT_FALSE(original_directory.empty());
    original_directory.pop_back();

    const fs::path test_directory{ fs::temp_directory_path() /
                                   ("simplcity-cli-" + std::to_string(::getpid())) };
    std::error_code cleanup_error;
    static_cast<void>(fs::remove_all(test_directory, cleanup_error));
    ASSERT_FALSE(cleanup_error);
    ASSERT_TRUE(fs::create_directory(test_directory));

    const auto enter{ execute(parser, "cd \"" + test_directory.generic_string() + '"', output) };
    ASSERT_TRUE(enter) << enter.message;

    auto result{ execute(parser, "mkdir -p nested/child", output) };
    EXPECT_TRUE(result) << result.message;
    EXPECT_TRUE(fs::is_directory(test_directory / "nested/child"));

    {
        std::ofstream file{ test_directory / "nested/child/source.txt" };
        ASSERT_TRUE(file.is_open());
        file << "hello from the CLI\n"
             << "nothing to see\n"
             << "HELLO again\n";
        ASSERT_TRUE(file);
    }

    result = execute(parser, "cat nested/child/source.txt", output);
    EXPECT_TRUE(result) << result.message;
    EXPECT_EQ(output.str(), "hello from the CLI\nnothing to see\nHELLO again\n");

    {
        std::ofstream first{ test_directory / "wild-a.txt" };
        std::ofstream second{ test_directory / "wild-b.txt" };
        std::ofstream hidden{ test_directory / ".wild-hidden.txt" };
        ASSERT_TRUE(first.is_open());
        ASSERT_TRUE(second.is_open());
        ASSERT_TRUE(hidden.is_open());
        first << "matching alpha\n";
        second << "matching beta\n";
        hidden << "hidden\n";
    }

    result = execute(parser, "echo wild-*.txt", output);
    EXPECT_TRUE(result) << result.message;
    EXPECT_EQ(output.str(), "wild-a.txt wild-b.txt\n");

    result = execute(parser, R"(echo "wild-*.txt" wild-\*.txt)", output);
    EXPECT_TRUE(result) << result.message;
    EXPECT_EQ(output.str(), "wild-*.txt wild-*.txt\n");

    result = execute(parser, "echo missing-*.txt", output);
    EXPECT_TRUE(result) << result.message;
    EXPECT_EQ(output.str(), "missing-*.txt\n");

    result = execute(parser, "echo .wild-*.txt", output);
    EXPECT_TRUE(result) << result.message;
    EXPECT_EQ(output.str(), ".wild-hidden.txt\n");

    result = execute(parser, "ls wild-*.txt", output);
    EXPECT_TRUE(result) << result.message;
    EXPECT_EQ(output.str(), "wild-a.txt\nwild-b.txt\n");

    result = execute(parser, "cat wild-*.txt", output);
    EXPECT_TRUE(result) << result.message;
    EXPECT_EQ(output.str(), "matching alpha\nmatching beta\n");

    result = execute(parser, "grep -n matching wild-*.txt", output);
    EXPECT_TRUE(result) << result.message;
    EXPECT_EQ(output.str(), "wild-a.txt:1:matching alpha\nwild-b.txt:1:matching beta\n");

    result = execute(parser, "mkdir wild-copies wild-moved", output);
    EXPECT_TRUE(result) << result.message;
    result = execute(parser, "cp wild-*.txt wild-copies", output);
    EXPECT_TRUE(result) << result.message;
    EXPECT_TRUE(fs::is_regular_file(test_directory / "wild-copies/wild-a.txt"));
    EXPECT_TRUE(fs::is_regular_file(test_directory / "wild-copies/wild-b.txt"));
    result = execute(parser, "mv wild-copies/wild-*.txt wild-moved", output);
    EXPECT_TRUE(result) << result.message;
    EXPECT_FALSE(fs::exists(test_directory / "wild-copies/wild-a.txt"));
    EXPECT_FALSE(fs::exists(test_directory / "wild-copies/wild-b.txt"));
    EXPECT_TRUE(fs::is_regular_file(test_directory / "wild-moved/wild-a.txt"));
    EXPECT_TRUE(fs::is_regular_file(test_directory / "wild-moved/wild-b.txt"));

    ASSERT_TRUE(fs::create_directory(test_directory / "wild-dir-a"));
    ASSERT_TRUE(fs::create_directory(test_directory / "wild-dir-b"));
    result = execute(parser, "rmdir wild-dir-*", output);
    EXPECT_TRUE(result) << result.message;
    EXPECT_FALSE(fs::exists(test_directory / "wild-dir-a"));
    EXPECT_FALSE(fs::exists(test_directory / "wild-dir-b"));

    result = execute(parser, "rm wild-*.txt", output);
    EXPECT_TRUE(result) << result.message;
    EXPECT_FALSE(fs::exists(test_directory / "wild-a.txt"));
    EXPECT_FALSE(fs::exists(test_directory / "wild-b.txt"));
    EXPECT_TRUE(fs::exists(test_directory / ".wild-hidden.txt"));
    ASSERT_TRUE(fs::remove(test_directory / ".wild-hidden.txt"));
    result = execute(parser, "rm wild-moved/wild-*.txt", output);
    EXPECT_TRUE(result) << result.message;
    result = execute(parser, "rmdir wild-copies wild-moved", output);
    EXPECT_TRUE(result) << result.message;

    result = execute(parser, "cat nested/child/source.txt | grep -in hello", output);
    EXPECT_TRUE(result) << result.message;
    EXPECT_EQ(output.str(), "1:hello from the CLI\n3:HELLO again\n");

    result = execute(parser, "echo hello embedded world | grep embedded", output);
    EXPECT_TRUE(result) << result.message;
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_EQ(output.str(), "hello embedded world\n");

    result = execute(parser, "grep absent nested/child/source.txt", output);
    EXPECT_TRUE(result) << result.message;
    EXPECT_EQ(result.exit_code, 1);
    EXPECT_TRUE(output.str().empty());

    result = execute(parser, "echo first > generated.txt", output);
    EXPECT_TRUE(result) << result.message;
    result = execute(parser, "echo second >> generated.txt", output);
    EXPECT_TRUE(result) << result.message;
    result = execute(parser, "cat generated.txt", output);
    EXPECT_TRUE(result) << result.message;
    EXPECT_EQ(output.str(), "first\nsecond\n");

    result = execute(parser, "cp nested/child/source.txt nested/child/copy.txt", output);
    EXPECT_TRUE(result) << result.message;
    EXPECT_TRUE(fs::is_regular_file(test_directory / "nested/child/copy.txt"));

    result = execute(parser, "mv nested/child/copy.txt nested/child/moved.txt", output);
    EXPECT_TRUE(result) << result.message;
    EXPECT_FALSE(fs::exists(test_directory / "nested/child/copy.txt"));
    EXPECT_TRUE(fs::is_regular_file(test_directory / "nested/child/moved.txt"));

    result = execute(parser, "touch nested/child/empty.txt", output);
    EXPECT_TRUE(result) << result.message;
    EXPECT_TRUE(fs::is_regular_file(test_directory / "nested/child/empty.txt"));

    const auto old_timestamp{ fs::file_time_type::clock::now() - std::chrono::hours{ 24 } };
    fs::last_write_time(test_directory / "nested/child/empty.txt", old_timestamp);
    result = execute(parser, "touch nested/child/empty.txt", output);
    EXPECT_TRUE(result) << result.message;
    EXPECT_GT(fs::last_write_time(test_directory / "nested/child/empty.txt"), old_timestamp);

    result = execute(parser, "ls -al nested/child", output);
    EXPECT_TRUE(result) << result.message;
    EXPECT_NE(output.str().find("source.txt"), std::string::npos);
    EXPECT_NE(output.str().find("moved.txt"), std::string::npos);
    EXPECT_NE(output.str().find("empty.txt"), std::string::npos);

    result = execute(parser, "rm nested/child/moved.txt", output);
    EXPECT_TRUE(result) << result.message;
    result = execute(parser, "rm nested/child/source.txt", output);
    EXPECT_TRUE(result) << result.message;
    result = execute(parser, "rm nested/child/empty.txt", output);
    EXPECT_TRUE(result) << result.message;
    result = execute(parser, "rm generated.txt", output);
    EXPECT_TRUE(result) << result.message;
    result = execute(parser, "rmdir nested/child", output);
    EXPECT_TRUE(result) << result.message;
    result = execute(parser, "rmdir nested", output);
    EXPECT_TRUE(result) << result.message;

    result = execute(parser, "rm -f missing", output);
    EXPECT_TRUE(result) << result.message;

    result = execute(parser, "mkdir \"\"", output);
    EXPECT_EQ(result.error, cli::ExecutionError::CallbackFailed);
    EXPECT_EQ(result.exit_code, 2);

    result = execute(parser, "rm -r .", output);
    EXPECT_EQ(result.error, cli::ExecutionError::CallbackFailed);
    EXPECT_TRUE(fs::is_directory(test_directory));

    const auto leave{ execute(parser, "cd \"" + original_directory + '"', output) };
    EXPECT_TRUE(leave) << leave.message;
    EXPECT_EQ(fs::remove_all(test_directory), 1U);
}

TEST(CliUtilities, PrintsHeapInformation)
{
    auto& parser{ cli::registry() };
    cli::utilities::setup();

    std::ostringstream output;
    const auto result{ execute(parser, "heap", output) };

    ASSERT_TRUE(result) << result.message;
    EXPECT_NE(output.str().find("Allocator"), std::string::npos);
    EXPECT_NE(output.str().find("glibc"), std::string::npos);
    EXPECT_NE(output.str().find("Capacity [bytes]"), std::string::npos);
    EXPECT_NE(output.str().find("Reserved [bytes]"), std::string::npos);
    EXPECT_NE(output.str().find("Used [bytes]"), std::string::npos);
    EXPECT_NE(output.str().find("Available [bytes]"), std::string::npos);
    EXPECT_NE(output.str().find("Heap-Usage Bar"), std::string::npos);
}
