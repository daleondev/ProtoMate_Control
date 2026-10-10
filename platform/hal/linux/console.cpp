#include "console.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <system_error>

namespace
{
    constexpr std::size_t QUEUE_CAPACITY{ 256U };

    enum class Mode : std::uint8_t
    {
        PassThrough,
        Buffering,
        Interactive,
        Plain,
    };

    // This state is constant-initialized so HAL diagnostics can use it before
    // ThreadX initializes the project's std::mutex implementation.
    struct ConsoleState
    {
        pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
        std::array<linux_console::Message, QUEUE_CAPACITY> queue{};
        std::size_t head{}, count{}, dropped{};
        Mode mode{ Mode::PassThrough };
    };

    auto console_state() noexcept -> ConsoleState&
    {
        static constinit ConsoleState state;
        return state;
    }

    class Lock
    {
      public:
        Lock(Lock&&) = delete;
        auto operator=(Lock&&) -> Lock& = delete;

        Lock() noexcept
          : m_locked{ pthread_mutex_lock(&console_state().mutex) == 0 }
        {
        }

        ~Lock()
        {
            if (m_locked) {
                static_cast<void>(pthread_mutex_unlock(&console_state().mutex));
            }
        }

        Lock(const Lock&) = delete;
        auto operator=(const Lock&) -> Lock& = delete;

        [[nodiscard]] explicit operator bool() const noexcept { return m_locked; }

      private:
        bool m_locked;
    };

    auto write(FILE* stream, std::string_view text) noexcept -> void
    {
        if (!text.empty()) {
            static_cast<void>(std::fwrite(text.data(), sizeof(char), text.size(), stream));
        }
    }

    auto enqueue(linux_console::Channel channel, std::string_view text, bool end_of_record) noexcept -> void
    {
        if (console_state().count == console_state().queue.size()) {
            console_state().head = (console_state().head + 1U) % console_state().queue.size();
            --console_state().count;
            ++console_state().dropped;
        }

        auto& message{ console_state().queue[(console_state().head + console_state().count) %
                                             console_state().queue.size()] };
        message.channel = channel;
        message.size = text.size();
        message.end_of_record = end_of_record;
        if (!text.empty()) {
            std::memcpy(message.text.data(), text.data(), text.size());
        }
        ++console_state().count;
    }

    auto make_drop_message(std::size_t amount) noexcept -> linux_console::Message
    {
        linux_console::Message message{
            .channel = linux_console::Channel::System,
            .end_of_record = true,
        };
        constexpr std::string_view prefix{ "[console] console_state().dropped " };
        constexpr std::string_view suffix{ " queued output chunks\n" };

        std::memcpy(message.text.data(), prefix.data(), prefix.size());
        char* const number_begin{ message.text.data() + prefix.size() };
        char* const number_end{ message.text.data() + message.text.size() - suffix.size() };
        const auto result{ std::to_chars(number_begin, number_end, amount) };
        const std::size_t number_size{ result.ec == std::errc{}
                                         ? static_cast<std::size_t>(result.ptr - number_begin)
                                         : 0U };
        std::memcpy(number_begin + number_size, suffix.data(), suffix.size());
        message.size = prefix.size() + number_size + suffix.size();
        return message;
    }

    auto flush_queue(FILE* stream) noexcept -> void
    {
        if (console_state().dropped != 0U) {
            const auto message{ make_drop_message(console_state().dropped) };
            write(stream, { message.text.data(), message.size });
            console_state().dropped = 0U;
        }

        while (console_state().count != 0U) {
            const auto& message{ console_state().queue[console_state().head] };
            write(stream, { message.text.data(), message.size });
            console_state().head = (console_state().head + 1U) % console_state().queue.size();
            --console_state().count;
        }
        static_cast<void>(std::fflush(stream));
    }
}

namespace linux_console
{
    auto begin_buffering() noexcept -> void
    {
        Lock lock;
        if (lock && console_state().mode == Mode::PassThrough) {
            console_state().mode = Mode::Buffering;
        }
    }

    auto activate_interactive() noexcept -> void
    {
        Lock lock;
        if (lock && console_state().mode != Mode::Plain) {
            console_state().mode = Mode::Interactive;
        }
    }

    auto activate_plain() noexcept -> void
    {
        Lock lock;
        if (!lock) {
            return;
        }
        console_state().mode = Mode::Plain;
        flush_queue(stderr);
    }

    auto publish(Channel channel, std::string_view text) noexcept -> void
    {
        Lock lock;
        if (!lock) {
            return;
        }

        if (console_state().mode == Mode::PassThrough) {
            write(stdout, text);
            static_cast<void>(std::fflush(stdout));
            return;
        }
        if (console_state().mode == Mode::Plain) {
            write(stderr, text);
            static_cast<void>(std::fflush(stderr));
            return;
        }

        if (text.empty()) {
            enqueue(channel, {}, true);
            return;
        }

        while (!text.empty()) {
            const std::size_t chunk_size{ std::min(text.size(), MESSAGE_PAYLOAD_SIZE) };
            const bool last{ chunk_size == text.size() };
            enqueue(channel, text.substr(0U, chunk_size), last);
            text.remove_prefix(chunk_size);
        }
    }

    auto drain(std::span<Message> output) noexcept -> std::size_t
    {
        Lock lock;
        if (!lock || output.empty() || console_state().mode != Mode::Interactive) {
            return 0U;
        }

        std::size_t written{};
        if (console_state().dropped != 0U) {
            output[written++] = make_drop_message(console_state().dropped);
            console_state().dropped = 0U;
        }

        while (written < output.size() && console_state().count != 0U) {
            output[written++] = console_state().queue[console_state().head];
            console_state().head = (console_state().head + 1U) % console_state().queue.size();
            --console_state().count;
        }
        return written;
    }
}
