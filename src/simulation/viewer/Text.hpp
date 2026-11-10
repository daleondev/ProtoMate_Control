#pragma once
#include "Gl.hpp"
#include <string_view>
#include <vector>
namespace simulation::viewer
{
    inline constexpr unsigned GLYPH_ROWS{ 7 }, GLYPH_COLUMNS{ 5 }, CHARACTER_ADVANCE{ 6 }, LINE_ADVANCE{ 10 };
    // Each literal is one five-bit raster row, not an algorithm parameter.
    // NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
    inline std::array<unsigned, GLYPH_ROWS> glyph(char c)
    {
        switch (c) {
            case 'A':
                return { 14, 17, 17, 31, 17, 17, 17 };
            case 'B':
                return { 30, 17, 17, 30, 17, 17, 30 };
            case 'C':
                return { 14, 17, 16, 16, 16, 17, 14 };
            case 'D':
                return { 30, 17, 17, 17, 17, 17, 30 };
            case 'E':
                return { 31, 16, 16, 30, 16, 16, 31 };
            case 'F':
                return { 31, 16, 16, 30, 16, 16, 16 };
            case 'G':
                return { 14, 17, 16, 23, 17, 17, 15 };
            case 'H':
                return { 17, 17, 17, 31, 17, 17, 17 };
            case 'I':
                return { 14, 4, 4, 4, 4, 4, 14 };
            case 'J':
                return { 7, 2, 2, 2, 18, 18, 12 };
            case 'K':
                return { 17, 18, 20, 24, 20, 18, 17 };
            case 'L':
                return { 16, 16, 16, 16, 16, 16, 31 };
            case 'M':
                return { 17, 27, 21, 21, 17, 17, 17 };
            case 'N':
                return { 17, 25, 25, 21, 19, 19, 17 };
            case 'O':
                return { 14, 17, 17, 17, 17, 17, 14 };
            case 'P':
                return { 30, 17, 17, 30, 16, 16, 16 };
            case 'Q':
                return { 14, 17, 17, 17, 21, 18, 13 };
            case 'R':
                return { 30, 17, 17, 30, 20, 18, 17 };
            case 'S':
                return { 15, 16, 16, 14, 1, 1, 30 };
            case 'T':
                return { 31, 4, 4, 4, 4, 4, 4 };
            case 'U':
                return { 17, 17, 17, 17, 17, 17, 14 };
            case 'V':
                return { 17, 17, 17, 17, 17, 10, 4 };
            case 'W':
                return { 17, 17, 17, 21, 21, 21, 10 };
            case 'X':
                return { 17, 17, 10, 4, 10, 17, 17 };
            case 'Y':
                return { 17, 17, 10, 4, 4, 4, 4 };
            case 'Z':
                return { 31, 1, 2, 4, 8, 16, 31 };
            case '0':
                return { 14, 17, 19, 21, 25, 17, 14 };
            case '1':
                return { 4, 12, 4, 4, 4, 4, 14 };
            case '2':
                return { 14, 17, 1, 2, 4, 8, 31 };
            case '3':
                return { 30, 1, 1, 14, 1, 1, 30 };
            case '4':
                return { 2, 6, 10, 18, 31, 2, 2 };
            case '5':
                return { 31, 16, 16, 30, 1, 1, 30 };
            case '6':
                return { 14, 16, 16, 30, 17, 17, 14 };
            case '7':
                return { 31, 1, 2, 4, 8, 8, 8 };
            case '8':
                return { 14, 17, 17, 14, 17, 17, 14 };
            case '9':
                return { 14, 17, 17, 15, 1, 1, 14 };
            case '.':
                return { 0, 0, 0, 0, 0, 6, 6 };
            case '-':
                return { 0, 0, 0, 31, 0, 0, 0 };
            case ':':
                return { 0, 6, 6, 0, 6, 6, 0 };
            case '/':
                return { 1, 1, 2, 4, 8, 16, 16 };
            case '=':
                return { 0, 0, 31, 0, 31, 0, 0 };
            default:
                return {};
        }
    }
    // NOLINTEND(cppcoreguidelines-avoid-magic-numbers,readability-magic-numbers)
    inline std::vector<Vertex> text_vertices(std::string_view text, float x, float y, float scale)
    {
        std::vector<Vertex> result{};
        const auto left{ x };
        for (char c : text) {
            if (c == '\n') {
                x = left;
                y += LINE_ADVANCE * scale;
                continue;
            }
            if (c >= 'a' && c <= 'z') {
                c = static_cast<char>(c - 'a' + 'A');
            }
            const auto rows{ glyph(c) };
            for (unsigned row{}; row < GLYPH_ROWS; ++row) {
                for (unsigned col{}; col < GLYPH_COLUMNS; ++col) {
                    if (rows[row] & (1U << (GLYPH_COLUMNS - 1 - col))) {
                        const glm::vec3 a{ x + (static_cast<float>(col) * scale),
                                           y + (static_cast<float>(row) * scale),
                                           0 };
                        const glm::vec3 b{ a[0] + scale, a[1], 0 };
                        const glm::vec3 c0{ a[0] + scale, a[1] + scale, 0 };
                        const glm::vec3 d{ a[0], a[1] + scale, 0 };
                        for (auto p : { a, b, c0, a, c0, d }) {
                            result.push_back({ p, { 0, 0, 1 } });
                        }
                    }
                }
            }
            x += CHARACTER_ADVANCE * scale;
        }
        return result;
    }
}
