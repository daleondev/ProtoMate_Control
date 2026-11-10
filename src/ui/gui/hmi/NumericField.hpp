#pragma once
#include "Model.hpp"
#include <limits>
#include <optional>
namespace ui::gui::hmi
{
    // Form validation only. IO still validates the complete command against
    // authoritative state, limits, ownership and configuration at admission.
    struct NumericField
    {
        const char* name{ "Position" };
        double minimum{ -std::numeric_limits<double>::max() };
        double maximum{ std::numeric_limits<double>::max() };
        bool minimum_exclusive{}, maximum_exclusive{};
        static NumericField forModel(const Model& model);
        bool signedValue() const { return minimum < 0; }
        std::optional<double> parse(const char* text) const;
        void describe(std::span<char> text) const;
    };
}
