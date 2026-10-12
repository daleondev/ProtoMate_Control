#pragma once

#include <stop_token>

namespace pnm::msg { class Bus; }
namespace ui::cli { class Parser; }

namespace ui::cli::utilities
{
    // Application-owned parser and UI bus. The token cancels remote diagnostics
    // when the UI application stops. Repeated setup on the same parser is a no-op;
    // the first successful call supplies its bus and token. Set up modules before running the CLI.
    void setup(Parser& parser, pnm::msg::Bus& bus, std::stop_token stop = {});
}
