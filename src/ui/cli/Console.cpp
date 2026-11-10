#include "Console.hpp"
#include "Parser.hpp"
#include "hal/console.hpp"
#include <functional>

#if defined(HAL_PLATFORM_LINUX)
#include "linux/Console.hpp"
#endif

#include <chrono>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace ui::cli
{
    void write_log(std::string_view line)
    {
#if defined(HAL_PLATFORM_LINUX)
        terminal::write_log(line);
#else
        static_cast<void>(std::fwrite(line.data(), 1, line.size(), stderr));
#endif
    }

    namespace
    {
        class StreamLine
        {
          public:
            StreamLine(Parser& parser, bool echo)
              : m_parser{ parser }
              , m_echo{ echo }
            {
            }
            void close()
            {
                if (!m_line.empty() || m_overflow) {
                    execute();
                }
                std::cout.flush();
            }
            void reset()
            {
                m_line.clear();
                m_afterCr = false;
                m_overflow = false;
                std::cout << "\r\n> " << std::flush;
            }
            bool character(char character)
            {
                if (character == '\n' && m_afterCr) {
                    m_afterCr = false;
                    return true;
                }
                m_afterCr = character == '\r';
                if (character == '\x03' || character == '\x04') {
                    std::cout << '\n' << std::flush;
                    return false;
                }
                if (character == '\r' || character == '\n') {
                    if (m_echo) {
                        std::cout << '\n';
                    }
                    execute();
                    std::cout << "> " << std::flush;
                }
                else if (character == '\b' || character == '\x7f') {
                    erase();
                }
                else if (!m_overflow) {
                    append(character);
                }
                return true;
            }

          private:
            void execute()
            {
                if (m_overflow) {
                    std::cout << "error: command line exceeds 4096 bytes\n";
                }
                else {
                    const auto result{ m_parser.get().execute(m_line, std::cout) };
                    if (result.error != ExecutionError::None && result.error != ExecutionError::EmptyInput) {
                        std::cout << "error: " << result.message << '\n';
                    }
                }
                m_line.clear();
                m_overflow = false;
            }
            void erase()
            {
                if (m_line.empty() || m_overflow) {
                    return;
                }
                m_line.pop_back();
                if (m_echo) {
                    std::cout << "\b \b" << std::flush;
                }
            }
            void append(char character)
            {
                if (m_line.size() == MAX_LINE_SIZE) {
                    m_overflow = true;
                    return;
                }
                m_line.push_back(character);
                if (m_echo) {
                    std::cout << character << std::flush;
                }
            }
            static constexpr std::size_t MAX_LINE_SIZE{ 4096 };
            std::reference_wrapper<Parser> m_parser;
            bool m_echo;
            std::string m_line{};
            bool m_afterCr{}, m_overflow{};
        };
    }
    void run_stream(Parser& parser, const std::stop_token& stop, bool echo)
    {
        constexpr auto input_interval{ std::chrono::milliseconds{ 10 } };
        StreamLine line{ parser, echo };
        std::cout << "> " << std::flush;
        while (!stop.stop_requested()) {
            const auto input{ hal::console::try_read() };
            switch (input.state) {
                case hal::console::InputState::Idle:
                    std::this_thread::sleep_for(input_interval);
                    break;
                case hal::console::InputState::Closed:
                    line.close();
                    return;
                case hal::console::InputState::Reset:
                    line.reset();
                    break;
                case hal::console::InputState::Error:
                    throw std::runtime_error("console input failed");
                case hal::console::InputState::Character:
                    if (!line.character(input.character)) {
                        return;
                    }
                    break;
            }
        }
    }

    void run(Parser& parser, const std::stop_token& stop)
    {
#if defined(HAL_PLATFORM_LINUX)
        terminal::run(parser, stop);
#else
        run_stream(parser, stop, true);
#endif
    }
}
