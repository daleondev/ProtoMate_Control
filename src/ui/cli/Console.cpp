#include "Console.hpp"
#include "Parser.hpp"
#include "hal/console.hpp"

#if defined(HAL_PLATFORM_LINUX)
#include "linux/Console.hpp"
#endif

#include <chrono>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace ui::cli
{
    void writeLog(std::string_view line)
    {
#if defined(HAL_PLATFORM_LINUX)
        terminal::write_log(line);
#else
        static_cast<void>(std::fwrite(line.data(), 1, line.size(), stderr));
#endif
    }

    void runStream(Parser& parser, std::stop_token stop, bool echo)
    {
        constexpr auto INPUT_INTERVAL{ std::chrono::milliseconds{ 10 } };
        constexpr std::size_t MAX_LINE_SIZE{ 4096 };
        std::string line;
        bool after_cr{}, overflow{};
        const auto execute = [&] {
            if (overflow) {
                std::cout << "error: command line exceeds 4096 bytes\n";
            }
            else {
                const auto result{ parser.execute(line, std::cout) };
                if (result.error != ExecutionError::None && result.error != ExecutionError::EmptyInput) {
                    std::cout << "error: " << result.message << '\n';
                }
            }
            line.clear();
            overflow = false;
        };
        std::cout << "> " << std::flush;
        while (!stop.stop_requested()) {
            const auto input{ hal::console::try_read() };
            switch (input.state) {
                case hal::console::InputState::Idle:
                    std::this_thread::sleep_for(INPUT_INTERVAL);
                    continue;
                case hal::console::InputState::Closed:
                    if (!line.empty() || overflow) { execute(); }
                    std::cout.flush();
                    return;
                case hal::console::InputState::Error:
                    throw std::runtime_error("console input failed");
                case hal::console::InputState::Character:
                    break;
            }
            const char character{ input.character };
            if (character == '\n' && after_cr) {
                after_cr = false;
                continue;
            }
            after_cr = character == '\r';
            if (character == '\x03' || character == '\x04') {
                std::cout << '\n' << std::flush;
                return;
            }
            if (character == '\r' || character == '\n') {
                if (echo) { std::cout << '\n'; }
                execute();
                std::cout << "> " << std::flush;
            }
            else if (character == '\b' || character == '\x7f') {
                if (!line.empty() && !overflow) {
                    line.pop_back();
                    if (echo) { std::cout << "\b \b" << std::flush; }
                }
            }
            else if (!overflow) {
                if (line.size() == MAX_LINE_SIZE) {
                    overflow = true;
                }
                else {
                    line.push_back(character);
                    if (echo) { std::cout << character << std::flush; }
                }
            }
        }
    }

    void run(Parser& parser, std::stop_token stop)
    {
#if defined(HAL_PLATFORM_LINUX)
        terminal::configure_logging();
        terminal::run(parser, stop);
#else
        runStream(parser, stop, true);
#endif
    }
}
