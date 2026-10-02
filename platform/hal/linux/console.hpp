#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace linux_console
{
    enum class Channel : std::uint8_t
    {
        hal,
        log,
        command,
        system,
    };

    inline constexpr std::size_t MESSAGE_PAYLOAD_SIZE{ 1024U };

    struct Message
    {
        Channel channel{ Channel::system };
        std::array<char, MESSAGE_PAYLOAD_SIZE> text{};
        std::size_t size{};
        bool end_of_record{};
    };

    /**
     * Start retaining output for the application's interactive frontend.
     *
     * This is safe to call before ThreadX and the custom C++ threading runtime
     * have been initialized.
     */
    auto begin_buffering() noexcept -> void;

    /**
     * Route subsequent output to the interactive queue.
     */
    auto activate_interactive() noexcept -> void;

    /**
     * Flush queued output to stderr and route subsequent output there.
     */
    auto activate_plain() noexcept -> void;

    /**
     * Publish one complete output record.
     *
     * Long records are split into fixed-size queue messages. The operation is
     * bounded and drops the oldest queued data if the frontend falls behind.
     */
    auto publish(Channel channel, std::string_view text) noexcept -> void;

    /**
     * Remove up to output.size() messages from the interactive queue.
     */
    [[nodiscard]] auto drain(std::span<Message> output) noexcept -> std::size_t;
}
