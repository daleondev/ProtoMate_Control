#pragma once
#include <array>
#include <format>
#include <span>
#include <utility>

namespace ui::services
{
    namespace text_capacity
    {
        inline constexpr unsigned LABEL{ 96 }, CONTROL{ 48 }, MESSAGE{ 160 }, DETAIL{ 192 }, NAME{ 40 },
          SHORT{ 32 }, NUMBER{ 24 };
    }
    template<unsigned N>
    using Text = std::array<char, N>;

    // Bounded, allocation-free presentation formatting, always NUL terminated.
    template<class... Args>
    void format(std::span<char> output, std::format_string<Args...> pattern, Args&&... args)
    {
        if (output.empty()) {
            return;
        }
        const auto result{ std::format_to_n(
          output.data(), output.size() - 1, pattern, std::forward<Args>(args)...) };
        *result.out = '\0';
    }
}
