#include "../system_threads.hpp"

#include "Parser.hpp"
#include "filesystem.hpp"
#include "utilities.hpp"

#if defined(HAL_PLATFORM_LINUX)
#include "linux/Console.hpp"
#endif

#include <iostream>

auto system_threads::cli() -> void
{
    auto& parser{ cli::registry() };

    cli::filesystem::setup();
    cli::utilities::setup();

#if defined(HAL_PLATFORM_LINUX)
    cli::terminal::run(parser);
#else
    parser.run(std::cin, std::cout);
#endif
}
