#pragma once

namespace cli
{
    class Parser;

    namespace terminal
    {
        /**
         * Route Pneumo's default Linux sinks through the console broker.
         */
        auto configureLogging() -> void;

        /**
         * Run the ncurses frontend on a terminal, or the stream frontend when
         * stdin/stdout are redirected or the terminal is unsupported.
         */
        auto run(Parser& parser) -> void;
    }
}
