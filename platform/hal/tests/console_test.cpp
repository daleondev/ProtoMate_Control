#include "hal/linux/console.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>

TEST(ConsoleBroker, BuffersCompleteAndChunkedRecordsForTheInteractiveFrontend)
{
    linux_console::begin_buffering();
    linux_console::publish(linux_console::Channel::hal, "startup\n");

    const std::string long_output(linux_console::MESSAGE_PAYLOAD_SIZE + 17U, 'x');
    linux_console::publish(linux_console::Channel::command, long_output);
    linux_console::activate_interactive();

    std::array<linux_console::Message, 4U> messages{};
    const std::size_t received{ linux_console::drain(messages) };
    ASSERT_EQ(received, 3U);

    EXPECT_EQ(messages[0].channel, linux_console::Channel::hal);
    EXPECT_EQ(std::string_view(messages[0].text.data(), messages[0].size), "startup\n");
    EXPECT_TRUE(messages[0].end_of_record);

    EXPECT_EQ(messages[1].channel, linux_console::Channel::command);
    EXPECT_EQ(messages[1].size, linux_console::MESSAGE_PAYLOAD_SIZE);
    EXPECT_FALSE(messages[1].end_of_record);

    EXPECT_EQ(messages[2].channel, linux_console::Channel::command);
    EXPECT_EQ(messages[2].size, 17U);
    EXPECT_TRUE(messages[2].end_of_record);
    EXPECT_EQ(std::string_view(messages[2].text.data(), messages[2].size),
              std::string_view(long_output).substr(linux_console::MESSAGE_PAYLOAD_SIZE));

    EXPECT_EQ(linux_console::drain(messages), 0U);
    linux_console::activate_plain();
}
