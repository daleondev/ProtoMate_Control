#include "Widgets.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <texts/TextKeysAndLanguages.hpp>
#include <touchgfx/hal/HAL.hpp>
namespace ui::gui::hmi
{
    void Panel::draw(const touchgfx::Rect& area) const
    {
        auto absolute = getAbsoluteRect();
        auto paint = [&](int x, int y, int w, int h, std::uint32_t rgb) {
            touchgfx::Rect r{ static_cast<std::int16_t>(x),
                              static_cast<std::int16_t>(y),
                              static_cast<std::int16_t>(w),
                              static_cast<std::int16_t>(h) };
            r &= area;
            if (!r.isEmpty()) {
                r.x += absolute.x;
                r.y += absolute.y;
                touchgfx::HAL::lcd().fillRect(r, rgb, 255);
            }
        };
        const int radiusValue = std::min<int>(radius, std::min(getWidth(), getHeight()) / 2);
        for (int y = 0; y < radiusValue; ++y) {
            const double dy = radiusValue - y - .5;
            const int inset = radiusValue - int(std::sqrt(radiusValue * radiusValue - dy * dy));
            paint(inset, y, getWidth() - 2 * inset, 1, border);
            paint(inset, getHeight() - y - 1, getWidth() - 2 * inset, 1, border);
            if (y > 0) {
                paint(inset + 1, y, getWidth() - 2 * inset - 2, 1, color);
                paint(inset + 1, getHeight() - y - 1, getWidth() - 2 * inset - 2, 1, color);
            }
        }
        paint(0, radiusValue, getWidth(), getHeight() - 2 * radiusValue, border);
        paint(1, radiusValue, getWidth() - 2, getHeight() - 2 * radiusValue, color);
    }
    template<std::size_t Capacity>
    BasicLabel<Capacity>::BasicLabel()
    {
        setWildcard(m_text.data());
        setTypedText(touchgfx::TypedText(T_CAPTIONVALUE));
        setColor(0xe7eef5);
        setWideTextAction(touchgfx::WIDE_TEXT_NONE);
    }
    template<std::size_t Capacity>
    void BasicLabel<Capacity>::large(bool value)
    {
        if (value == m_large)
            return;
        invalidate();
        m_large = value;
        m_wrapWidth = -1;
        setTypedText(touchgfx::TypedText(value ? T_BODYVALUE : T_CAPTIONVALUE));
        invalidate();
    }
    template<std::size_t Capacity>
    void BasicLabel<Capacity>::text(const char* value)
    {
        std::array<touchgfx::Unicode::UnicodeChar, Capacity> source{}, next{};
        touchgfx::Unicode::fromUTF8(
          reinterpret_cast<const std::uint8_t*>(value), source.data(), source.size() - 1);
        if (m_wrapWidth == getWidth() && source == m_text)
            return;
        m_wrapWidth = getWidth();
        // Explicit bounded wrapping avoids the SDK engine's wildcard word-wrap
        // truncation. Measure the actual font, not an estimated character width.
        const auto* font = getTypedText().getFont();
        unsigned out{}, lineStart{}, space{};
        int width{};
        touchgfx::Unicode::UnicodeChar previous{};
        for (unsigned i = 0; source[i] && out + 1 < next.size(); ++i) {
            const auto c = source[i];
            if (c == '\n') {
                next[out++] = c;
                lineStart = out;
                space = 0;
                width = 0;
                previous = 0;
                continue;
            }
            const auto* glyph = font->getGlyph(c);
            const int advance = glyph ? glyph->advance() + font->getKerning(previous, glyph) : 0;
            if (width + advance > getWidth() - 2 && out > lineStart) {
                if (space >= lineStart && space) {
                    next[space] = '\n';
                    lineStart = space + 1;
                    width = 0;
                    previous = 0;
                    for (unsigned j = lineStart; j < out; ++j) {
                        const auto* g = font->getGlyph(next[j]);
                        if (g)
                            width += g->advance() + font->getKerning(previous, g);
                        previous = next[j];
                    }
                }
                else {
                    next[out++] = '\n';
                    lineStart = out;
                    width = 0;
                    previous = 0;
                }
                space = 0;
                if (out + 1 >= next.size())
                    break;
            }
            next[out] = c;
            if (c == ' ')
                space = out;
            ++out;
            if (glyph)
                width += glyph->advance() + font->getKerning(previous, glyph);
            previous = c;
        }
        if (next != m_text) {
            invalidate();
            m_text = next;
            setTypedText(getTypedText());
            invalidate();
        }
    }
    template class BasicLabel<192>;
    template class BasicLabel<48>;
    void Icon::draw(const touchgfx::Rect& area) const
    {
        const auto origin = getAbsoluteRect();
        // Union the original 2x2 line pixels, then emit horizontal runs. This
        // preserves the icon exactly while avoiding one DMA transaction (and
        // worker wakeup) per overlapping Bresenham pixel.
        std::array<std::uint32_t, 26> rows{};
        auto line = [&](int x, int y, int ex, int ey) {
            int dx = std::abs(ex - x), sx = x < ex ? 1 : -1, dy = -std::abs(ey - y), sy = y < ey ? 1 : -1,
                error = dx + dy;
            for (;;) {
                for (int py = y; py < y + 2; ++py)
                    for (int px = x; px < x + 2; ++px)
                        if (px >= 0 && px < 26 && py >= 0 && py < 26)
                            rows[py] |= 1U << px;
                if (x == ex && y == ey)
                    break;
                const int twice = 2 * error;
                if (twice >= dy) {
                    error += dy;
                    x += sx;
                }
                if (twice <= dx) {
                    error += dx;
                    y += sy;
                }
            }
        };
        switch (kind) {
            case 0:
                line(1, 12, 12, 2);
                line(12, 2, 23, 12);
                line(5, 11, 5, 23);
                line(5, 23, 19, 23);
                line(19, 23, 19, 11);
                line(10, 23, 10, 16);
                line(10, 16, 14, 16);
                line(14, 16, 14, 23);
                break;
            case 1:
                line(2, 12, 22, 12);
                line(12, 2, 12, 22);
                line(2, 12, 6, 8);
                line(2, 12, 6, 16);
                line(22, 12, 18, 8);
                line(22, 12, 18, 16);
                line(12, 2, 8, 6);
                line(12, 2, 16, 6);
                line(12, 22, 8, 18);
                line(12, 22, 16, 18);
                break;
            case 2:
                for (int y : { 4, 12, 20 }) {
                    line(2, y, 5, y);
                    line(9, y, 23, y);
                }
                break;
            case 3:
                line(6, 2, 6, 22);
                line(18, 2, 18, 22);
                line(2, 7, 10, 7);
                line(14, 17, 22, 17);
                line(2, 5, 2, 9);
                line(10, 5, 10, 9);
                line(14, 15, 14, 19);
                line(22, 15, 22, 19);
                break;
            default:
                for (int i = 0; i < 8; ++i) {
                    const float a = float(i) * .785398f;
                    line(int(12 + 7 * std::cos(a)),
                         int(12 + 7 * std::sin(a)),
                         int(12 + 11 * std::cos(a)),
                         int(12 + 11 * std::sin(a)));
                }
                line(8, 8, 16, 8);
                line(16, 8, 16, 16);
                line(16, 16, 8, 16);
                line(8, 16, 8, 8);
                break;
        }
        for (int y = 0; y < 26;) {
            int end = y + 1;
            while (end < 26 && rows[end] == rows[y])
                ++end;
            for (int x = 0; x < 26;) {
                if (!(rows[y] & (1U << x))) {
                    ++x;
                    continue;
                }
                const int start = x;
                while (x < 26 && (rows[y] & (1U << x)))
                    ++x;
                touchgfx::Rect r{ static_cast<int16_t>(start), static_cast<int16_t>(y),
                                  static_cast<int16_t>(x - start), static_cast<int16_t>(end - y) };
                r &= area;
                if (!r.isEmpty()) {
                    r.x += origin.x;
                    r.y += origin.y;
                    touchgfx::HAL::lcd().fillRect(r, 0xcbddeb, 255);
                }
            }
            y = end;
        }
    }
    Control::Control()
    {
        setTouchable(true);
        add(m_panel);
        add(m_label);
        add(m_icon);
        m_icon.setVisible(false);
        layout(0, 0, 160);
    }
    void Control::icon(unsigned kind)
    {
        m_icon.kind = kind;
        m_icon.setPosition((getWidth() - 26) / 2, 7, 26, 26);
        m_icon.setVisible(true);
        m_label.setPosition(8, 37, getWidth() - 16, 30);
    }
    void Control::layout(int x, int y, int width, int height)
    {
        if (getX() == x && getY() == y && getWidth() == width && getHeight() == height)
            return;
        invalidate();
        setPosition(x, y, width, height);
        m_panel.setPosition(0, 0, width, height);
        m_label.setPosition(8, (height - 28) / 2, width - 16, 30);
        invalidate();
    }
    void Control::configure(const char* label, unsigned id, Callback cb, void* context)
    {
        if (id != m_id && m_pressed) {
            m_pressed = false;
            refresh();
        }
        m_label.text(label);
        m_id = id;
        m_callback = cb;
        m_context = context;
    }
    void Control::enabled(bool v)
    {
        if (v == m_enabled)
            return;
        m_enabled = v;
        setTouchable(v);
        m_pressed = false;
        refresh();
    }
    void Control::selected(bool v)
    {
        if (v == m_selected)
            return;
        m_selected = v;
        refresh();
    }
    void Control::danger(bool v)
    {
        if (v == m_danger)
            return;
        m_danger = v;
        refresh();
    }
    void Control::refresh()
    {
        m_panel.color = !m_enabled   ? 0x18232d
                        : m_pressed  ? 0x164b7f
                        : m_danger   ? 0x74333d
                        : m_selected ? 0x154e8a
                                     : 0x223443;
        m_panel.border = m_selected ? 0x329fff : m_danger ? 0xb35762 : 0x3a5061;
        m_label.setColor(m_enabled ? 0xe7eef5 : 0x718493);
        invalidate();
    }
    void Control::handleClickEvent(const touchgfx::ClickEvent& e)
    {
        if (!m_enabled)
            return;
        const bool inside = e.getX() >= 0 && e.getY() >= 0 && e.getX() < getWidth() && e.getY() < getHeight();
        const bool was = m_pressed;
        m_pressed = e.getType() == touchgfx::ClickEvent::PRESSED && inside;
        refresh();
        if (!m_callback)
            return;
        if (m_held) {
            if (m_pressed)
                m_callback(m_context, m_id, true);
            else if (was)
                m_callback(m_context, m_id, false);
        }
        else if (was && inside && e.getType() == touchgfx::ClickEvent::RELEASED)
            m_callback(m_context, m_id, true);
    }
    void Control::handleDragEvent(const touchgfx::DragEvent& e)
    {
        if (m_pressed &&
            (e.getNewX() < 0 || e.getNewY() < 0 || e.getNewX() >= getWidth() || e.getNewY() >= getHeight())) {
            m_pressed = false;
            refresh();
            if (m_held && m_callback)
                m_callback(m_context, m_id, false);
        }
    }
}
