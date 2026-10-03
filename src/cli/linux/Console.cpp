#include "Console.hpp"

#include "cli/Parser.hpp"
#include "hal/linux/console.hpp"
#include "pneumo/logging.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <clocale>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <curses.h>
#include <cwchar>
#include <cwctype>
#include <deque>
#include <iostream>
#include <memory>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{
    constexpr int INPUT_REFRESH_MILLISECONDS{ 50 };
    constexpr std::size_t MAX_LOG_LINES{ 2'000U };
    constexpr std::size_t DRAIN_BATCH_SIZE{ 64U };

    class ConsoleSink final : public pnm::log::SinkBase<ConsoleSink>
    {
      public:
        [[nodiscard]] auto isOpen() const -> bool override { return true; }

        auto open() -> pnm::Result<> override { return {}; }

        auto close() -> pnm::Result<> override { return {}; }

        auto write(std::span<const std::byte> data) -> pnm::Result<std::size_t> override
        {
            const auto* const characters{ reinterpret_cast<const char*>(data.data()) };
            linux_console::publish(linux_console::Channel::log, { characters, data.size() });
            return data.size();
        }

        auto flush() -> pnm::Result<> override { return {}; }
    };

    [[nodiscard]] auto plain_requested() noexcept -> bool
    {
        const char* const value{ std::getenv("SIMPLCITY_CLI_PLAIN") };
        return value != nullptr && value[0] != '\0' && std::string_view{ value } != "0";
    }

    [[nodiscard]] auto terminal_supported() noexcept -> bool
    {
        if (plain_requested() || ::isatty(STDIN_FILENO) == 0 || ::isatty(STDOUT_FILENO) == 0) {
            return false;
        }

        const char* const terminal{ std::getenv("TERM") };
        return terminal != nullptr && terminal[0] != '\0' && std::string_view{ terminal } != "dumb";
    }

    class TerminalSession
    {
      public:
        TerminalSession()
        {
            if (!terminal_supported()) {
                return;
            }

            m_screen = newterm(nullptr, stdout, stdin);
            if (m_screen == nullptr) {
                return;
            }

            if (raw() == ERR || noecho() == ERR || keypad(stdscr, true) == ERR) {
                static_cast<void>(endwin());
                delscreen(m_screen);
                m_screen = nullptr;
                return;
            }

            static_cast<void>(curs_set(1));
            static_cast<void>(set_escdelay(25));
            if (has_colors() && start_color() != ERR) {
                const short background{ static_cast<short>(use_default_colors() == ERR ? COLOR_BLACK : -1) };
                m_hasColors = init_pair(1, COLOR_CYAN, background) != ERR &&
                              init_pair(2, COLOR_GREEN, background) != ERR &&
                              init_pair(3, COLOR_YELLOW, background) != ERR &&
                              init_pair(4, COLOR_RED, background) != ERR;
            }

            linux_console::activate_interactive();
            m_active = true;
        }

        ~TerminalSession()
        {
            if (m_screen != nullptr) {
                if (m_active) {
                    static_cast<void>(curs_set(1));
                    static_cast<void>(echo());
                    static_cast<void>(noraw());
                    static_cast<void>(endwin());
                }
                delscreen(m_screen);
            }
            linux_console::activate_plain();
        }

        TerminalSession(const TerminalSession&) = delete;
        auto operator=(const TerminalSession&) -> TerminalSession& = delete;

        [[nodiscard]] auto active() const noexcept -> bool { return m_active; }
        [[nodiscard]] auto hasColors() const noexcept -> bool { return m_hasColors; }

      private:
        SCREEN* m_screen{};
        bool m_active{};
        bool m_hasColors{};
    };

    [[nodiscard]] auto widen(std::string_view input) -> std::wstring
    {
        std::wstring output;
        output.reserve(input.size());
        std::mbstate_t state{};

        while (!input.empty()) {
            wchar_t character{};
            const std::size_t consumed{ std::mbrtowc(&character, input.data(), input.size(), &state) };
            if (consumed == static_cast<std::size_t>(-1) || consumed == static_cast<std::size_t>(-2)) {
                output.push_back(L'\uFFFD');
                input.remove_prefix(1U);
                state = {};
                continue;
            }
            if (consumed == 0U) {
                output.push_back(L'?');
                input.remove_prefix(1U);
                state = {};
                continue;
            }

            if (character == L'\t') {
                output.append(4U, L' ');
            }
            else if (std::iswcntrl(character) == 0) {
                output.push_back(character);
            }
            else {
                output.push_back(L'?');
            }
            input.remove_prefix(consumed);
        }
        return output;
    }

    [[nodiscard]] auto narrow(std::wstring_view input) -> std::string
    {
        std::string output;
        output.reserve(input.size());
        std::mbstate_t state{};
        std::array<char, MB_LEN_MAX> buffer{};

        for (const wchar_t character : input) {
            const std::size_t written{ std::wcrtomb(buffer.data(), character, &state) };
            if (written == static_cast<std::size_t>(-1)) {
                output.push_back('?');
                state = {};
                continue;
            }
            output.append(buffer.data(), written);
        }
        return output;
    }

    [[nodiscard]] auto character_width(wchar_t character) noexcept -> int
    {
        const int width{ ::wcwidth(character) };
        return width > 0 ? width : 1;
    }

    [[nodiscard]] auto text_width(std::wstring_view text) noexcept -> int
    {
        int result{};
        for (const wchar_t character : text) {
            result += character_width(character);
        }
        return result;
    }

    [[nodiscard]] auto clip(std::wstring_view text, int width) -> std::wstring
    {
        if (width <= 0 || text.empty()) {
            return {};
        }
        if (text_width(text) <= width) {
            return std::wstring{ text };
        }

        std::wstring result;
        int used{};
        const int content_width{ std::max(0, width - 1) };
        for (const wchar_t character : text) {
            const int current{ character_width(character) };
            if (used + current > content_width) {
                break;
            }
            result.push_back(character);
            used += current;
        }
        result.push_back(L'\u2026');
        return result;
    }

    struct LogLine
    {
        linux_console::Channel channel;
        std::wstring text;
    };

    struct InputEvent
    {
        enum class Type : std::uint8_t
        {
            character,
            key,
        };

        Type type{ Type::character };
        wchar_t character{};
        int key{};
    };

    struct InputBatch
    {
        std::array<InputEvent, 64U> events{};
        std::size_t size{};
        bool closed{};
    };

    class TerminalInput
    {
      public:
        [[nodiscard]] auto read() -> InputBatch
        {
            InputBatch result;
            pollfd descriptor{
                .fd = STDIN_FILENO,
                .events = POLLIN,
                .revents = 0,
            };

            // A blocking host poll is invisible to ThreadX and can leave its
            // simulated current thread suspended indefinitely. Probe without
            // blocking; the caller yields through the ThreadX-aware C++ sleep.
            int poll_result{};
            for (std::size_t attempts{}; attempts < 16U; ++attempts) {
                poll_result = ::poll(&descriptor, 1U, 0);
                if (poll_result >= 0 || errno != EINTR) {
                    break;
                }
                descriptor.revents = 0;
            }
            if (poll_result == 0) {
                m_escape.clear();
                return result;
            }
            if (poll_result < 0) {
                result.closed = errno != EINTR;
                return result;
            }
            if ((descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
                result.closed = true;
                return result;
            }

            std::array<unsigned char, 64U> input{};
            ssize_t received{};
            do {
                received = ::read(STDIN_FILENO, input.data(), static_cast<unsigned long>(input.size()));
            } while (received < 0 && errno == EINTR);
            if (received == 0) {
                result.closed = true;
                return result;
            }
            if (received < 0) {
                result.closed = errno != EINTR && errno != EAGAIN;
                return result;
            }

            for (const unsigned char byte : std::span{ input }.first(static_cast<std::size_t>(received))) {
                consume(byte, result);
            }
            return result;
        }

      private:
        static auto emitCharacter(InputBatch& output, wchar_t character) -> void
        {
            if (output.size < output.events.size()) {
                output.events[output.size++] = {
                    .type = InputEvent::Type::character,
                    .character = character,
                };
            }
        }

        static auto emitKey(InputBatch& output, int key) -> void
        {
            if (output.size < output.events.size()) {
                output.events[output.size++] = {
                    .type = InputEvent::Type::key,
                    .key = key,
                };
            }
        }

        auto consume(unsigned char byte, InputBatch& output) -> void
        {
            constexpr unsigned char escape{ 0x1BU };
            if (!m_escape.empty()) {
                m_escape.push_back(static_cast<char>(byte));
                if (m_escape.size() == 2U && (byte == '[' || byte == 'O')) {
                    return;
                }
                if (byte >= 0x40U && byte <= 0x7EU) {
                    emitEscape(output);
                }
                else if (m_escape.size() == 16U) {
                    m_escape.clear();
                }
                return;
            }

            if (byte == escape) {
                m_escape.push_back(static_cast<char>(byte));
                return;
            }
            if (m_multibyte.empty() && byte < 0x80U) {
                emitCharacter(output, static_cast<wchar_t>(byte));
                return;
            }

            m_multibyte.push_back(static_cast<char>(byte));
            wchar_t character{};
            std::mbstate_t state{};
            const std::size_t converted{ std::mbrtowc(
              &character, m_multibyte.data(), m_multibyte.size(), &state) };
            if (converted == static_cast<std::size_t>(-2)) {
                return;
            }
            if (converted == static_cast<std::size_t>(-1)) {
                emitCharacter(output, L'\uFFFD');
            }
            else {
                emitCharacter(output, character);
            }
            m_multibyte.clear();
        }

        auto emitEscape(InputBatch& output) -> void
        {
            const std::string_view sequence{ m_escape };
            if (sequence == "\x1B[A" || sequence == "\x1BOA") {
                emitKey(output, KEY_UP);
            }
            else if (sequence == "\x1B[B" || sequence == "\x1BOB") {
                emitKey(output, KEY_DOWN);
            }
            else if (sequence == "\x1B[C" || sequence == "\x1BOC") {
                emitKey(output, KEY_RIGHT);
            }
            else if (sequence == "\x1B[D" || sequence == "\x1BOD") {
                emitKey(output, KEY_LEFT);
            }
            else if (sequence == "\x1B[H" || sequence == "\x1BOH" || sequence == "\x1B[1~") {
                emitKey(output, KEY_HOME);
            }
            else if (sequence == "\x1B[F" || sequence == "\x1BOF" || sequence == "\x1B[4~") {
                emitKey(output, KEY_END);
            }
            else if (sequence == "\x1B[3~") {
                emitKey(output, KEY_DC);
            }
            else if (sequence == "\x1B[5~") {
                emitKey(output, KEY_PPAGE);
            }
            else if (sequence == "\x1B[6~") {
                emitKey(output, KEY_NPAGE);
            }
            m_escape.clear();
        }

        std::string m_escape;
        std::string m_multibyte;
    };

    class InteractiveCli
    {
      public:
        InteractiveCli(cli::Parser& parser, bool has_colors)
          : m_parser{ parser }
          , m_hasColors{ has_colors }
        {
        }

        auto run() -> void
        {
            bool running{ true };
            bool dirty{ true };
            TerminalInput input;

            while (running) {
                dirty = resizeIfNeeded() || drainMessages() || dirty;
                if (dirty) {
                    render();
                    dirty = false;
                }

                const InputBatch batch{ input.read() };
                if (batch.closed) {
                    break;
                }
                if (batch.size == 0U) {
                    std::this_thread::sleep_for(std::chrono::milliseconds{ INPUT_REFRESH_MILLISECONDS });
                    continue;
                }
                for (const auto& event : std::span{ batch.events }.first(batch.size)) {
                    if (event.type == InputEvent::Type::key) {
                        dirty = handleKey(event.key) || dirty;
                    }
                    else {
                        running = handleCharacter(event.character);
                        dirty = true;
                    }
                    if (!running) {
                        break;
                    }
                }
            }
        }

      private:
        [[nodiscard]] static auto resizeIfNeeded() -> bool
        {
            winsize terminal_size{};
            if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &terminal_size) != 0 || terminal_size.ws_row == 0U ||
                terminal_size.ws_col == 0U) {
                return false;
            }

            int rows{};
            int columns{};
            getmaxyx(stdscr, rows, columns);
            if (rows == terminal_size.ws_row && columns == terminal_size.ws_col) {
                return false;
            }
            return resizeterm(terminal_size.ws_row, terminal_size.ws_col) != ERR;
        }

        [[nodiscard]] auto drainMessages() -> bool
        {
            bool changed{};
            std::array<linux_console::Message, DRAIN_BATCH_SIZE> messages{};
            for (std::size_t batch{}; batch < 8U; ++batch) {
                const std::size_t received{ linux_console::drain(messages) };
                for (const auto& message : std::span{ messages }.first(received)) {
                    changed = append(message) || changed;
                }
                if (received < messages.size()) {
                    break;
                }
            }
            return changed;
        }

        [[nodiscard]] auto append(const linux_console::Message& message) -> bool
        {
            const std::size_t channel{ static_cast<std::size_t>(message.channel) };
            auto& pending{ m_pending.at(channel) };
            bool committed{};

            for (const char character : std::string_view{ message.text.data(), message.size }) {
                if (character == '\n') {
                    commit(message.channel, pending);
                    committed = true;
                }
                else if (character != '\r') {
                    pending.push_back(character);
                }
            }

            if (message.end_of_record && !pending.empty()) {
                commit(message.channel, pending);
                committed = true;
            }
            return committed;
        }

        auto commit(linux_console::Channel channel, std::string& pending) -> void
        {
            m_lines.push_back({
              .channel = channel,
              .text = widen(pending),
            });
            pending.clear();

            if (m_scrollOffset != 0U) {
                ++m_scrollOffset;
            }
            if (m_lines.size() > MAX_LOG_LINES) {
                m_lines.pop_front();
                if (m_scrollOffset != 0U) {
                    --m_scrollOffset;
                }
            }
        }

        auto execute() -> void
        {
            const std::string line{ narrow(m_input) };
            if (!line.empty()) {
                linux_console::publish(linux_console::Channel::command, std::string{ "> " } + line + '\n');
                if (m_history.empty() || m_history.back() != m_input) {
                    m_history.push_back(m_input);
                }
            }

            m_historyIndex = m_history.size();
            m_historyDraft.clear();
            m_input.clear();
            m_cursor = 0U;

            std::ostringstream output;
            const cli::ExecutionResult execution{ m_parser.execute(line, output) };
            const std::string command_output{ std::move(output).str() };
            if (!command_output.empty()) {
                linux_console::publish(linux_console::Channel::command, command_output);
            }
            if (execution.error != cli::ExecutionError::None &&
                execution.error != cli::ExecutionError::EmptyInput) {
                linux_console::publish(linux_console::Channel::command, "error: " + execution.message + '\n');
            }
            m_scrollOffset = 0U;
        }

        [[nodiscard]] auto handleCharacter(wchar_t character) -> bool
        {
            switch (character) {
                case L'\n':
                case L'\r':
                    execute();
                    return true;
                case 1: // Ctrl-A
                    m_cursor = 0U;
                    return true;
                case 3: // Ctrl-C
                    return false;
                case 4: // Ctrl-D
                    if (m_cursor < m_input.size()) {
                        m_input.erase(m_cursor, 1U);
                        leaveHistory();
                    }
                    return true;
                case 5: // Ctrl-E
                    m_cursor = m_input.size();
                    return true;
                case 11: // Ctrl-K
                    m_input.erase(m_cursor);
                    leaveHistory();
                    return true;
                case 12: // Ctrl-L
                    clearok(stdscr, true);
                    return true;
                case 21: // Ctrl-U
                    m_input.erase(0U, m_cursor);
                    m_cursor = 0U;
                    leaveHistory();
                    return true;
                case 8:
                case 127:
                    backspace();
                    return true;
                default:
                    break;
            }

            if (std::iswprint(character) != 0) {
                m_input.insert(m_cursor, 1U, character);
                ++m_cursor;
                leaveHistory();
            }
            return true;
        }

        [[nodiscard]] auto handleKey(int key) -> bool
        {
            switch (key) {
                case KEY_ENTER:
                    execute();
                    break;
                case KEY_BACKSPACE:
                    backspace();
                    break;
                case KEY_DC:
                    if (m_cursor < m_input.size()) {
                        m_input.erase(m_cursor, 1U);
                        leaveHistory();
                    }
                    break;
                case KEY_LEFT:
                    if (m_cursor != 0U) {
                        --m_cursor;
                    }
                    break;
                case KEY_RIGHT:
                    if (m_cursor < m_input.size()) {
                        ++m_cursor;
                    }
                    break;
                case KEY_HOME:
                    m_cursor = 0U;
                    break;
                case KEY_END:
                    m_cursor = m_input.size();
                    break;
                case KEY_UP:
                    previousHistory();
                    break;
                case KEY_DOWN:
                    nextHistory();
                    break;
                case KEY_PPAGE:
                    scrollUp();
                    break;
                case KEY_NPAGE:
                    m_scrollOffset = m_scrollOffset > pageSize() ? m_scrollOffset - pageSize() : 0U;
                    break;
                case KEY_RESIZE:
                    break;
                default:
                    return false;
            }
            return true;
        }

        auto backspace() -> void
        {
            if (m_cursor != 0U) {
                m_input.erase(m_cursor - 1U, 1U);
                --m_cursor;
                leaveHistory();
            }
        }

        auto previousHistory() -> void
        {
            if (m_history.empty() || m_historyIndex == 0U) {
                return;
            }
            if (m_historyIndex == m_history.size()) {
                m_historyDraft = m_input;
            }
            --m_historyIndex;
            m_input = m_history[m_historyIndex];
            m_cursor = m_input.size();
        }

        auto nextHistory() -> void
        {
            if (m_historyIndex >= m_history.size()) {
                return;
            }
            ++m_historyIndex;
            m_input = m_historyIndex == m_history.size() ? m_historyDraft : m_history[m_historyIndex];
            m_cursor = m_input.size();
        }

        auto leaveHistory() -> void
        {
            m_historyIndex = m_history.size();
            m_historyDraft.clear();
        }

        [[nodiscard]] auto pageSize() const noexcept -> std::size_t
        {
            int rows{};
            int columns{};
            getmaxyx(stdscr, rows, columns);
            static_cast<void>(columns);
            return static_cast<std::size_t>(std::max(1, (rows - 2) / 2));
        }

        auto scrollUp() -> void
        {
            int rows{};
            int columns{};
            getmaxyx(stdscr, rows, columns);
            static_cast<void>(columns);
            const std::size_t visible{ static_cast<std::size_t>(std::max(0, rows - 2)) };
            const std::size_t maximum{ m_lines.size() > visible ? m_lines.size() - visible : 0U };
            m_scrollOffset = std::min(maximum, m_scrollOffset + pageSize());
        }

        [[nodiscard]] auto color(linux_console::Channel channel) const noexcept -> int
        {
            if (!m_hasColors) {
                return 0;
            }
            switch (channel) {
                case linux_console::Channel::hal:
                    return 1;
                case linux_console::Channel::log:
                    return 2;
                case linux_console::Channel::command:
                    return 3;
                case linux_console::Channel::system:
                    return 4;
            }
            return 0;
        }

        auto render() -> void
        {
            int rows{};
            int columns{};
            getmaxyx(stdscr, rows, columns);
            static_cast<void>(erase());

            if (rows < 3 || columns < 8) {
                constexpr std::string_view message{ "terminal too small" };
                static_cast<void>(mvaddnstr(0, 0, message.data(), std::min<int>(columns, message.size())));
                static_cast<void>(refresh());
                return;
            }

            const int log_rows{ rows - 2 };
            const std::size_t visible{ static_cast<std::size_t>(log_rows) };
            const std::size_t maximum_scroll{ m_lines.size() > visible ? m_lines.size() - visible : 0U };
            m_scrollOffset = std::min(m_scrollOffset, maximum_scroll);
            const std::size_t end{ m_lines.size() - m_scrollOffset };
            const std::size_t begin{ end > visible ? end - visible : 0U };

            int row{};
            for (std::size_t index{ begin }; index < end && row < log_rows; ++index, ++row) {
                const auto& line{ m_lines[index] };
                const int pair{ color(line.channel) };
                if (pair != 0) {
                    static_cast<void>(attron(COLOR_PAIR(pair)));
                }
                const std::wstring rendered{ clip(line.text, columns) };
                static_cast<void>(mvaddnwstr(row, 0, rendered.data(), static_cast<int>(rendered.size())));
                if (pair != 0) {
                    static_cast<void>(attroff(COLOR_PAIR(pair)));
                }
            }

            static_cast<void>(mvhline(rows - 2, 0, ACS_HLINE, columns));
            const std::string status{ m_scrollOffset == 0U
                                        ? " logs | PgUp/PgDn scroll | Ctrl-C close CLI "
                                        : " logs | viewing history | PgDn returns to latest " };
            static_cast<void>(
              mvaddnstr(rows - 2, 1, status.data(), std::min<int>(columns - 2, status.size())));

            constexpr std::string_view prompt{ "> " };
            static_cast<void>(mvaddnstr(rows - 1, 0, prompt.data(), prompt.size()));
            renderInput(rows - 1, static_cast<int>(prompt.size()), columns);
            static_cast<void>(refresh());
        }

        auto renderInput(int row, int prompt_width, int columns) -> void
        {
            const int available{ std::max(0, columns - prompt_width) };
            std::size_t first{ m_cursor };
            int before_cursor{};
            while (first != 0U) {
                const int width{ character_width(m_input[first - 1U]) };
                if (before_cursor + width > available - 1) {
                    break;
                }
                before_cursor += width;
                --first;
            }

            std::wstring visible;
            int used{};
            for (std::size_t index{ first }; index < m_input.size(); ++index) {
                const int width{ character_width(m_input[index]) };
                if (used + width > available) {
                    break;
                }
                visible.push_back(m_input[index]);
                used += width;
            }

            static_cast<void>(
              mvaddnwstr(row, prompt_width, visible.data(), static_cast<int>(visible.size())));
            const int cursor_column{ prompt_width + text_width(std::wstring_view{ m_input }.substr(
                                                      first, m_cursor - first)) };
            static_cast<void>(move(row, std::min(columns - 1, cursor_column)));
        }

        cli::Parser& m_parser;
        bool m_hasColors;
        std::deque<LogLine> m_lines;
        std::array<std::string, 4U> m_pending;
        std::size_t m_scrollOffset{};
        std::wstring m_input;
        std::size_t m_cursor{};
        std::vector<std::wstring> m_history;
        std::size_t m_historyIndex{};
        std::wstring m_historyDraft;
    };
}

extern "C" void runtime_application_prepare()
{
    static_cast<void>(std::setlocale(LC_CTYPE, ""));
    linux_console::begin_buffering();
}

namespace cli::terminal
{
    auto configureLogging() -> void
    {
        auto sink{ std::make_shared<ConsoleSink>() };
        if (!pnm::log::set_default_sink(pnm::log::Level::Trace, pnm::log::Level::Critical, std::move(sink))) {
            throw std::runtime_error{ "failed to configure the Linux console log sink" };
        }
    }

    auto run(Parser& parser) -> void
    {
        TerminalSession terminal;
        if (!terminal.active()) {
            linux_console::activate_plain();
            parser.run(std::cin, std::cout);
            return;
        }

        InteractiveCli frontend{ parser, terminal.hasColors() };
        frontend.run();
    }
}
