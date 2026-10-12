#include "ui/cli/Console.hpp"
#include "ui/cli/Parser.hpp"
#include "ui/cli/motor.hpp"
#include "hal/console.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <iostream>
#include <sstream>
#include <string_view>
#include <thread>
#include <unistd.h>

namespace
{
    using namespace std::chrono_literals;

    class CliConsole : public testing::Test
    {
      protected:
        void SetUp() override
        {
            saved = ::dup(STDIN_FILENO);
            ASSERT_GE(saved, 0);
            ASSERT_EQ(::pipe(pipe.data()), 0);
            ASSERT_GE(::dup2(pipe[0], STDIN_FILENO), 0);
            old_output = std::cout.rdbuf(output.rdbuf());
        }
        void TearDown() override
        {
            if (old_output) { std::cout.rdbuf(old_output); }
            if (saved >= 0) { ::dup2(saved, STDIN_FILENO); ::close(saved); }
            for (const int descriptor : pipe) {
                if (descriptor >= 0) { ::close(descriptor); }
            }
        }
        void send(std::string_view text)
        {
            ASSERT_EQ(::write(pipe[1], text.data(), text.size()), static_cast<ssize_t>(text.size()));
        }
        void eof()
        {
            ASSERT_EQ(::close(pipe[1]), 0);
            pipe[1] = -1;
        }
        void addEcho(ui::cli::Parser& parser)
        {
            ASSERT_TRUE(parser.registerCommand({
                .name = "say", .description = "test command",
                .arguments = { { .name = "value", .description = "text" } }, .flags = {},
                .callback = [](const ui::cli::Arguments& arguments, std::ostream& stream) {
                    stream << arguments.at(0) << '\n';
                    return 0;
                }
            }));
        }

        std::array<int, 2> pipe{ -1, -1 };
        int saved{ -1 };
        std::ostringstream output;
        std::streambuf* old_output{};
    };

    TEST_F(CliConsole, InputDistinguishesIdleCharactersAndEof)
    {
        EXPECT_EQ(hal::console::try_read().state, hal::console::InputState::Idle);
        send("x");
        const auto value{ hal::console::try_read() };
        EXPECT_EQ(value.state, hal::console::InputState::Character);
        EXPECT_EQ(value.character, 'x');
        eof();
        EXPECT_EQ(hal::console::try_read().state, hal::console::InputState::Closed);
    }

    TEST_F(CliConsole, CrLfEditingAndLastLineWithoutNewline)
    {
        ui::cli::Parser parser;
        addEcho(parser);
        send("say onX\be\r\nsay two\nsay three");
        eof();
        ui::cli::runStream(parser, {}, false);
        EXPECT_EQ(output.str(), "> one\n> two\n> three\n");
    }

    TEST_F(CliConsole, StopWhileWaitingDiscardsPartialCommand)
    {
        ui::cli::Parser parser;
        addEcho(parser);
        send("say must-not-execute");
        std::jthread worker([&](std::stop_token stop) { ui::cli::runStream(parser, stop, false); });
        std::this_thread::sleep_for(30ms);
        worker.request_stop();
        worker.join();
        EXPECT_EQ(output.str(), "> ");
    }

    TEST_F(CliConsole, ControlCDoesNotExecuteThePendingLine)
    {
        ui::cli::Parser parser;
        addEcho(parser);
        send("say must-not-execute\x03");
        ui::cli::runStream(parser, {}, false);
        EXPECT_EQ(output.str(), "> \n");
    }

    TEST_F(CliConsole, MotorModuleRemainsEmpty)
    {
        ui::cli::Parser parser;
        ui::cli::motor::setup(parser);
        EXPECT_FALSE(parser.contains("motor"));
        EXPECT_FALSE(parser.contains("motor status"));
        EXPECT_EQ(parser.execute("motor", output).error, ui::cli::ExecutionError::UnknownCommand);
    }
}
