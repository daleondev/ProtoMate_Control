#pragma once
#include <stop_token>

namespace ui::motion
{
    class Client;
}

namespace ui::cli
{
    class Parser;
}

namespace ui::cli::motor
{
    // Registers once per parser. The application-owned client outlives the parser.
    void setup(Parser& parser, ui::motion::Client& client, const std::stop_token& stop = {});
}
