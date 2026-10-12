#pragma once

#include <stop_token>

namespace pnm::msg { class Bus; }
namespace ui::cli { class Parser; }

namespace ui::cli::utilities
{
    // Application-owned parser and UI bus. The token cancels remote diagnostics
    // when the UI application stops. Registration happens before running the CLI.
    void setup(Parser& parser, pnm::msg::Bus& bus, std::stop_token stop = {});
    // Standalone registry for local commands/tests, without a remote endpoint.
    auto setup() -> void;
}
