#include "Widgets.hpp"
#include "Layout.hpp"
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
    namespace
    {
        constexpr unsigned OPAQUE_ALPHA{ 255 };
        constexpr int ICON_GROUP_HEIGHT{ 34 }, ICON_SIZE{ 26 }, CONTROL_PADDING{ 8 };
        constexpr unsigned DEFAULT_CONTROL_WIDTH{ 160 };
        constexpr double HALF_PIXEL{ .5 };
        constexpr unsigned alignment_column(touchgfx::Alignment alignment)
        {
            if (alignment == touchgfx::LEFT) {
                return 0;
            }
            if (alignment == touchgfx::CENTER) {
                return 1;
            }
            return 2;
        }
        int measure_line(const touchgfx::Font& font,
                         std::span<const touchgfx::Unicode::UnicodeChar> line,
                         touchgfx::Unicode::UnicodeChar& previous)
        {
            int width{};
            previous = 0;
            for (const auto c : line) {
                if (const auto* glyph{ font.getGlyph(c) }) {
                    width += glyph->advance() + font.getKerning(previous, glyph);
                }
                previous = c;
            }
            return width;
        }
    }
    void Panel::draw(const touchgfx::Rect& area) const
    {
        auto absolute{ getAbsoluteRect() };
        auto paint{ [&](layout::Bounds bounds, std::uint32_t rgb) {
            touchgfx::Rect r{ static_cast<std::int16_t>(bounds.x),
                              static_cast<std::int16_t>(bounds.y),
                              static_cast<std::int16_t>(bounds.width),
                              static_cast<std::int16_t>(bounds.height) };
            r &= area;
            if (!r.isEmpty()) {
                r.x = static_cast<std::int16_t>(r.x + absolute.x);
                r.y = static_cast<std::int16_t>(r.y + absolute.y);
                touchgfx::HAL::lcd().fillRect(r, rgb, OPAQUE_ALPHA);
            }
        } };
        const int radius_value{ std::min<int>(static_cast<int>(radius),
                                              std::min(getWidth(), getHeight()) / 2) };
        for (int y{ 0 }; y < radius_value; ++y) {
            const double dy{ radius_value - y - HALF_PIXEL };
            const int inset{ radius_value - int(std::sqrt((radius_value * radius_value) - (dy * dy))) };
            paint({ .x = inset, .y = y, .width = getWidth() - (2 * inset), .height = 1 }, border);
            paint({ .x = inset, .y = getHeight() - y - 1, .width = getWidth() - (2 * inset), .height = 1 },
                  border);
            if (y > 0) {
                paint({ .x = inset + 1, .y = y, .width = getWidth() - (2 * inset) - 2, .height = 1 }, color);
                paint({ .x = inset + 1,
                        .y = getHeight() - y - 1,
                        .width = getWidth() - (2 * inset) - 2,
                        .height = 1 },
                      color);
            }
        }
        paint({ .x = 0, .y = radius_value, .width = getWidth(), .height = getHeight() - (2 * radius_value) },
              border);
        paint(
          { .x = 1, .y = radius_value, .width = getWidth() - 2, .height = getHeight() - (2 * radius_value) },
          color);
    }
    template<std::size_t Capacity>
    BasicLabel<Capacity>::BasicLabel(bool paint_only)
      : m_paintOnly{ paint_only }
    {
        setWildcard(m_text.data());
        setTypedText(touchgfx::TypedText(T_CAPTIONVALUE));
        setColor(theme::LABEL);
        setWideTextAction(touchgfx::WIDE_TEXT_NONE);
    }
    template<std::size_t Capacity>
    void BasicLabel<Capacity>::typography()
    {
        constexpr std::array<std::array<touchgfx::TypedTextId, 3>, 4> ids{
            { { T_CAPTIONLEFT, T_CAPTIONCENTER, T_CAPTIONRIGHT },
              { T_BODYLEFT, T_BODYCENTER, T_BODYRIGHT },
              { T_HEADINGLEFT, T_HEADINGCENTER, T_HEADINGRIGHT },
              { T_SMALLLEFT, T_SMALLCENTER, T_SMALLRIGHT } }
        };
        const unsigned column{ alignment_column(m_alignment) };
        const auto id{ ids[m_size][column] };
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
        std::array<touchgfx::Unicode::UnicodeChar, Capacity> source{};
        std::array<touchgfx::Unicode::UnicodeChar, Capacity> next{};
        touchgfx::Unicode::fromUTF8(
          // The SDK accepts UTF-8 as unsigned bytes; character-byte aliasing is permitted.
          // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
          reinterpret_cast<const std::uint8_t*>(value),
          source.data(),
          source.size() - 1);
        if (m_wrapWidth == getWidth() && source == m_text) {
            return;
        }
        m_wrapWidth = getWidth();
        // Explicit bounded wrapping avoids the SDK engine's wildcard word-wrap
        // truncation. Measure the actual font, not an estimated character width.
        const auto* font{ getTypedText().getFont() };
        unsigned out{};
        unsigned line_start{};
        unsigned space{};
        int width{};
        touchgfx::Unicode::UnicodeChar previous{};
        for (unsigned i{ 0 }; source[i] && out + 1 < next.size(); ++i) {
            const auto c{ source[i] };
            if (c == '\n') {
                next[out++] = c;
                line_start = out;
                space = 0;
                width = 0;
                previous = 0;
                continue;
            }
            const auto* glyph{ font->getGlyph(c) };
            const int advance{ glyph ? glyph->advance() + font->getKerning(previous, glyph) : 0 };
            if (width + advance > getWidth() - 2 && out > line_start) {
                if (space >= line_start && space) {
                    next[space] = '\n';
                    line_start = space + 1;
                    width = 0;
                    previous = 0;
                    width =
                      measure_line(*font, std::span(next).subspan(line_start, out - line_start), previous);
                }
                else {
                    next[out++] = '\n';
                    line_start = out;
                    width = 0;
                    previous = 0;
                }
                space = 0;
                if (out + 1 >= next.size()) {
                    break;
                }
            }
            next[out] = c;
            if (c == ' ') {
                space = out;
            }
            ++out;
            if (glyph) {
                width += glyph->advance() + font->getKerning(previous, glyph);
            }
            previous = c;
        }
        if (next != m_text) {
            damage();
            m_text = next;
            setTypedText(getTypedText());
            damage();
        }
    }
    template class BasicLabel<text_capacity::LABEL>;
    template class BasicLabel<text_capacity::CONTROL>;
    void Icon::draw(const touchgfx::Rect& area) const
    {
        constexpr std::array<touchgfx::BitmapId, 6> icons{ BITMAP_NAV_HOME_ID,    BITMAP_NAV_MOVE_ID,
                                                           BITMAP_NAV_PROGRAM_ID, BITMAP_NAV_IO_ID,
                                                           BITMAP_NAV_ALARMS_ID,  BITMAP_NAV_SETTINGS_ID };
        touchgfx::Image image{};
        image.setBitmap(touchgfx::Bitmap(icons[std::min<std::size_t>(kind, icons.size() - 1)]));
        const auto origin{ getAbsoluteRect() };
        image.setXY(static_cast<std::int16_t>(origin.x), static_cast<std::int16_t>(origin.y));
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
        layout(0, 0, DEFAULT_CONTROL_WIDTH);
        refresh();
    }
    void Control::alignLabel()
    {
        const int icon_group{ m_icon.isVisible() ? ICON_GROUP_HEIGHT : 0 }; // 26 px icon + 8 px gap
        const int height{ std::min<int>(m_label.getTextHeight(), getHeight() - icon_group - 4) };
        const int top{ (getHeight() - icon_group - height) / 2 };
        m_label.setPosition(
          static_cast<std::int16_t>(m_icon.isVisible() ? 2 : CONTROL_PADDING),
          static_cast<std::int16_t>(top + icon_group),
          static_cast<std::int16_t>(getWidth() - (m_icon.isVisible() ? 4 : 2 * CONTROL_PADDING)),
          static_cast<std::int16_t>(height));
        m_icon.setPosition(static_cast<std::int16_t>((getWidth() - ICON_SIZE) / 2),
                           static_cast<std::int16_t>(top),
                           ICON_SIZE,
                           ICON_SIZE);
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
        if (getX() == x && getY() == y && getWidth() == width && getHeight() == height) {
            return;
        }
        invalidate();
        setPosition(static_cast<std::int16_t>(x),
                    static_cast<std::int16_t>(y),
                    static_cast<std::int16_t>(width),
                    static_cast<std::int16_t>(height));
        m_panel.setPosition(0, 0, static_cast<std::int16_t>(width), static_cast<std::int16_t>(height));
        m_label.setWidth(static_cast<std::int16_t>(width - (m_icon.isVisible() ? 4 : 2 * CONTROL_PADDING)));
        alignLabel();
        invalidate();
    }
    void Control::configure(const char* label, Action id, Callback cb, void* context)
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
    void Control::appearance(Appearance style)
    {
        const auto [color, radius, large]{ style };
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
        if (v == m_enabled) {
            return;
        }
        m_enabled = v;
        setTouchable(v);
        m_pressed = false;
        refresh();
    }
    void Control::selected(bool v)
    {
        if (v == m_selected) {
            return;
        }
        m_selected = v;
        refresh();
    }
    void Control::danger(bool v)
    {
        if (v == m_danger) {
            return;
        }
        m_danger = v;
        refresh();
    }
    std::uint32_t Control::panelColor() const
    {
        if (!m_enabled) {
            return theme::DISABLED;
        }
        if (m_pressed) {
            return theme::PRESSED;
        }
        if (m_danger) {
            return theme::DANGER;
        }
        if (m_selected) {
            return theme::SELECTED;
        }
        return m_tint ? m_tint : theme::CONTROL;
    }
    std::uint32_t Control::panelBorder() const
    {
        if (m_selected) {
            return theme::SELECTED_BORDER;
        }
        if (m_danger) {
            return theme::DANGER_BORDER;
        }
        return theme::CONTROL_BORDER;
    }
    void Control::refresh()
    {
        m_panel.color = panelColor();
        m_panel.border = panelBorder();
        m_label.setColor(m_enabled ? theme::TEXT : theme::DISABLED_TEXT);
        invalidate();
    }
    void Control::handleClickEvent(const touchgfx::ClickEvent& e)
    {
        if (!m_enabled) {
            return;
        }
        const bool inside{ e.getX() >= 0 && e.getY() >= 0 && e.getX() < getWidth() &&
                           e.getY() < getHeight() };
        const bool was{ m_pressed };
        m_pressed = e.getType() == touchgfx::ClickEvent::PRESSED && inside;
        refresh();
        if (!m_callback) {
            return;
        }
        if (m_held) {
            if (m_pressed) {
                m_callback(m_context, m_id, true);
            }
            else if (was) {
                m_callback(m_context, m_id, false);
            }
        }
        else if (was && inside && e.getType() == touchgfx::ClickEvent::RELEASED) {
            m_callback(m_context, m_id, true);
        }
    }
    void Control::handleDragEvent(const touchgfx::DragEvent& e)
    {
        if (m_pressed &&
            (e.getNewX() < 0 || e.getNewY() < 0 || e.getNewX() >= getWidth() || e.getNewY() >= getHeight())) {
            m_pressed = false;
            refresh();
            if (m_held && m_callback) {
                m_callback(m_context, m_id, false);
            }
        }
    }
}
