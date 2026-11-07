#include "Widgets.hpp"
#include <BitmapDatabase.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <texts/TextKeysAndLanguages.hpp>
#include <touchgfx/hal/HAL.hpp>
#include <touchgfx/widgets/Image.hpp>
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
    BasicLabel<Capacity>::BasicLabel(bool paintOnly)
      : m_paintOnly{ paintOnly }
    {
        setWildcard(m_text.data());
        setTypedText(touchgfx::TypedText(T_CAPTIONVALUE));
        setColor(0xe7eef5);
        setWideTextAction(touchgfx::WIDE_TEXT_NONE);
    }
    template<std::size_t Capacity>
    void BasicLabel<Capacity>::typography()
    {
        constexpr touchgfx::TypedTextId ids[][3]{ { T_CAPTIONLEFT, T_CAPTIONCENTER, T_CAPTIONRIGHT },
                                                  { T_BODYLEFT, T_BODYCENTER, T_BODYRIGHT },
                                                  { T_HEADINGLEFT, T_HEADINGCENTER, T_HEADINGRIGHT },
                                                  { T_SMALLLEFT, T_SMALLCENTER, T_SMALLRIGHT } };
        const unsigned column = m_alignment == touchgfx::LEFT ? 0 : m_alignment == touchgfx::CENTER ? 1 : 2;
        const auto id = ids[m_size][column];
        if (getTypedText().getId() != id) {
            damage();
            setTypedText(touchgfx::TypedText(id));
            m_wrapWidth = -1;
            damage();
        }
    }
    template<std::size_t Capacity>
    void BasicLabel<Capacity>::style(Typography size, touchgfx::Alignment alignment)
    {
        m_size = unsigned(size);
        m_alignment = alignment;
        typography();
    }
    template<std::size_t Capacity>
    void BasicLabel<Capacity>::align(touchgfx::Alignment value)
    {
        m_alignment = value;
        typography();
    }
    template<std::size_t Capacity>
    void BasicLabel<Capacity>::heading()
    {
        m_size = 2;
        typography();
    }
    template<std::size_t Capacity>
    void BasicLabel<Capacity>::small()
    {
        m_size = 3;
        typography();
    }
    template<std::size_t Capacity>
    void BasicLabel<Capacity>::large(bool value)
    {
        m_size = value ? 1 : 0;
        typography();
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
            damage();
            m_text = next;
            setTypedText(getTypedText());
            damage();
        }
    }
    template class BasicLabel<96>;
    template class BasicLabel<48>;
    void Icon::draw(const touchgfx::Rect& area) const
    {
        constexpr touchgfx::BitmapId icons[]{
            BITMAP_NAV_HOME_ID, BITMAP_NAV_MOVE_ID,   BITMAP_NAV_PROGRAM_ID,
            BITMAP_NAV_IO_ID,   BITMAP_NAV_ALARMS_ID, BITMAP_NAV_SETTINGS_ID
        };
        touchgfx::Image image;
        image.setBitmap(touchgfx::Bitmap(icons[std::min(kind, 5U)]));
        const auto origin = getAbsoluteRect();
        image.setXY(origin.x, origin.y);
        image.draw(area);
    }
    Control::Control()
    {
        setTouchable(true);
        add(m_panel);
        add(m_label);
        add(m_icon);
        m_icon.setVisible(false);
        m_label.align(touchgfx::CENTER);
        layout(0, 0, 160);
        refresh();
    }
    void Control::alignLabel()
    {
        const int iconGroup = m_icon.isVisible() ? 34 : 0; // 26 px icon + 8 px gap
        const int height = std::min<int>(m_label.getTextHeight(), getHeight() - iconGroup - 4);
        const int top = (getHeight() - iconGroup - height) / 2;
        m_label.setPosition(
          m_icon.isVisible() ? 2 : 8, top + iconGroup, getWidth() - (m_icon.isVisible() ? 4 : 16), height);
        m_icon.setPosition((getWidth() - 26) / 2, top, 26, 26);
    }
    void Control::icon(unsigned kind)
    {
        m_icon.kind = kind;
        m_icon.setVisible(true);
        m_label.small();
        alignLabel();
    }
    void Control::layout(int x, int y, int width, int height)
    {
        if (getX() == x && getY() == y && getWidth() == width && getHeight() == height)
            return;
        invalidate();
        setPosition(x, y, width, height);
        m_panel.setPosition(0, 0, width, height);
        m_label.setWidth(width - (m_icon.isVisible() ? 4 : 16));
        alignLabel();
        invalidate();
    }
    void Control::configure(const char* label, unsigned id, Callback cb, void* context)
    {
        if (id != m_id && m_pressed) {
            m_pressed = false;
            refresh();
        }
        m_label.text(label);
        alignLabel();
        m_id = id;
        m_callback = cb;
        m_context = context;
    }
    void Control::appearance(std::uint32_t color, unsigned radius, bool large)
    {
        tint(color);
        rounded(radius);
        m_label.style(large ? Typography::Heading : Typography::Caption, touchgfx::CENTER);
    }
    void Control::tint(std::uint32_t color)
    {
        if (m_tint != color) {
            m_tint = color;
            refresh();
        }
    }
    void Control::rounded(unsigned radius)
    {
        if (m_panel.radius != radius) {
            m_panel.radius = radius;
            invalidate();
        }
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
        m_panel.color = !m_enabled   ? theme::DISABLED
                        : m_pressed  ? theme::PRESSED
                        : m_danger   ? theme::DANGER
                        : m_selected ? theme::SELECTED
                        : m_tint     ? m_tint
                                     : theme::CONTROL;
        m_panel.border = m_selected ? theme::SELECTED_BORDER
                         : m_danger ? theme::DANGER_BORDER
                                    : theme::CONTROL_BORDER;
        m_label.setColor(m_enabled ? theme::TEXT : theme::DISABLED_TEXT);
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
