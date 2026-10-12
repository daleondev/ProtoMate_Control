#pragma once

#include <stop_token>
#include <string_view>

namespace ui::cli
{
    class Parser;

    namespace terminal
    {
        auto write_log(std::string_view line) -> void;

        /**
         * Run the ncurses frontend on a terminal, or the stream frontend when
         * stdin/stdout are redirected or the terminal is unsupported.
         */
        auto run(Parser& parser, std::stop_token stop) -> void;
    }
}
