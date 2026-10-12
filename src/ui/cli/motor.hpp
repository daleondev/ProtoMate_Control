#pragma once

namespace ui::cli
{
    class Parser;
}

namespace ui::cli::motor
{
    // Reserved for the future ICC client; currently registers no commands.
    void setup(Parser& parser);
}
