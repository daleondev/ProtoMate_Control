#pragma once

namespace ui { class SystemState; }
namespace ui::cli { class Parser; }

namespace ui::cli::state
{
    // The application-owned state must outlive the parser. Repeated setup on
    // the same parser is a no-op, retaining its original state source.
    void setup(Parser& parser, const SystemState& system);
}
