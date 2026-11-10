#include "Table.hpp"
#include "Layout.hpp"
#include "ui/gui/frontend/Theme.hpp"
#include <algorithm>
#include <touchgfx/hal/HAL.hpp>
namespace ui::gui::hmi
{
    namespace
    {
        constexpr int MAX_ROW_HEIGHT{ 34 }, NAME_PERCENT{ 26 }, PERCENT{ 100 }, CELL_PADDING{ 12 };
        constexpr int BAR_BOTTOM{ 5 }, BAR_HEIGHT{ 3 }, WARNING_PERCENT{ 90 };
        constexpr int SLIDER_INSET{ 16 }, SLIDER_TRACK_Y{ 26 }, SLIDER_TRACK_HEIGHT{ 6 };
        constexpr int SLIDER_THUMB_HALF_WIDTH{ 8 }, SLIDER_THUMB_Y{ 17 }, SLIDER_THUMB_HEIGHT{ 24 };
        constexpr std::uint8_t OPAQUE_ALPHA{ 255 };
        constexpr std::uint32_t HEADER_COLOR{ 0x223645 }, ODD_ROW_COLOR{ 0x192a36 },
          EVEN_ROW_COLOR{ 0x172630 };
        void fill(const touchgfx::Widget& widget,
                  const touchgfx::Rect& area,
                  layout::Bounds bounds,
                  std::uint32_t color)
        {
            touchgfx::Rect r{ static_cast<std::int16_t>(bounds.x),
                              static_cast<std::int16_t>(bounds.y),
                              static_cast<std::int16_t>(bounds.width),
                              static_cast<std::int16_t>(bounds.height) };
            r &= area;
            if (!r.isEmpty()) {
                const auto origin{ widget.getAbsoluteRect() };
                r.x = static_cast<std::int16_t>(r.x + origin.x);
                r.y = static_cast<std::int16_t>(r.y + origin.y);
                touchgfx::HAL::lcd().fillRect(r, color, OPAQUE_ALPHA);
            }
        }
        struct Cell
        {
            layout::Bounds bounds{};
            const char* text{};
            std::uint32_t color{};
            touchgfx::Alignment alignment{};
        };
        void draw_cell(const touchgfx::Widget& widget, const touchgfx::Rect& area, const Cell& cell)
        {
            ControlLabel label{ true };
            const auto origin{ widget.getAbsoluteRect() };
            layout::position(label,
                             { .x = origin.x + cell.bounds.x + CELL_PADDING,
                               .y = origin.y + cell.bounds.y,
                               .width = cell.bounds.width - (2 * CELL_PADDING),
                               .height = cell.bounds.height });
            label.align(cell.alignment);
            label.text(cell.text);
            label.setColor(cell.color);
            label.setY(static_cast<std::int16_t>(origin.y + cell.bounds.y +
                                                 ((cell.bounds.height - label.getTextHeight()) / 2)));
            auto clip{ area };
            clip.x = static_cast<std::int16_t>(clip.x + origin.x);
            clip.y = static_cast<std::int16_t>(clip.y + origin.y);
            clip &= label.getRect();
            clip.x = static_cast<std::int16_t>(clip.x - label.getX());
            clip.y = static_cast<std::int16_t>(clip.y - label.getY());
            if (!clip.isEmpty()) {
                label.draw(clip);
            }
        }
        std::uint32_t background(unsigned row)
        {
            if (!row) {
                return HEADER_COLOR;
            }
            return row % 2 ? ODD_ROW_COLOR : EVEN_ROW_COLOR;
        }
    }
    void Table::drawRow(const touchgfx::Rect& area, const Row& row, RowGeometry geometry) const
    {
        for (unsigned c{ 0 }; c < m_data.columns; ++c) {
            draw_cell(*this,
                      area,
                      { .bounds = { .x = c ? geometry.name_width + (int(c - 1) * geometry.cell_width) : 0,
                                    .y = geometry.y,
                                    .width = c ? geometry.cell_width : geometry.name_width,
                                    .height = geometry.height },
                        .text = c ? row.values[c - 1].data() : row.name.data(),
                        .color = geometry.header ? theme::MUTED : row.color,
                        .alignment = c ? touchgfx::RIGHT : touchgfx::LEFT });
        }
        if (!geometry.header && row.percent >= 0) {
            const int width{ std::min(row.percent, PERCENT) * (geometry.cell_width - 2 * CELL_PADDING) /
                             PERCENT };
            fill(*this,
                 area,
                 { .x = getWidth() - geometry.cell_width + CELL_PADDING,
                   .y = geometry.y + geometry.height - BAR_BOTTOM,
                   .width = width,
                   .height = BAR_HEIGHT },
                 row.percent >= WARNING_PERCENT ? theme::AMBER : theme::ACCENT);
        }
    }
    void Table::draw(const touchgfx::Rect& area) const
    {
        fill(*this, area, { .width = getWidth(), .height = getHeight() }, theme::SURFACE);
        // Keep header and row positions stable on a partially filled final page.
        const int row_height{ std::min(MAX_ROW_HEIGHT, getHeight() / int(m_data.count + 1)) };
        const int name_width{ m_data.columns == 2 ? getWidth() / 2 : getWidth() * NAME_PERCENT / PERCENT };
        const int cell_width{ (getWidth() - name_width) / int(std::max(1U, m_data.columns - 1)) };
        for (unsigned i{ 0 }; i <= m_data.count; ++i) {
            const auto& row{ i ? m_data.rows[i - 1] : m_data.heading };
            const int y{ int(i) * row_height };
            fill(*this, area, { .y = y, .width = getWidth(), .height = row_height }, background(i));
            fill(*this, area, { .y = y + row_height - 1, .width = getWidth(), .height = 1 }, theme::BORDER);
            drawRow(area,
                    row,
                    { .y = y,
                      .height = row_height,
                      .name_width = name_width,
                      .cell_width = cell_width,
                      .header = i == 0 });
        }
    }
    void Slider::value(unsigned v)
    {
        v = std::min(v, unsigned(PERCENT));
        if (v != m_value) {
            m_value = v;
            invalidate();
        }
    }
    void Slider::draw(const touchgfx::Rect& area) const
    {
        const int x{ SLIDER_INSET + (int(m_value) * (getWidth() - 2 * SLIDER_INSET) / PERCENT) };
        fill(*this,
             area,
             { .x = SLIDER_INSET,
               .y = SLIDER_TRACK_Y,
               .width = getWidth() - (2 * SLIDER_INSET),
               .height = SLIDER_TRACK_HEIGHT },
             theme::BORDER);
        fill(*this,
             area,
             { .x = SLIDER_INSET,
               .y = SLIDER_TRACK_Y,
               .width = x - SLIDER_INSET,
               .height = SLIDER_TRACK_HEIGHT },
             theme::ACCENT);
        fill(*this,
             area,
             { .x = x - SLIDER_THUMB_HALF_WIDTH,
               .y = SLIDER_THUMB_Y,
               .width = 2 * SLIDER_THUMB_HALF_WIDTH,
               .height = SLIDER_THUMB_HEIGHT },
             theme::TEXT);
    }
    void Slider::input(int x)
    {
        value(unsigned(std::clamp(
          (x - SLIDER_INSET) * PERCENT / std::max(1, int(getWidth()) - (2 * SLIDER_INSET)), 0, PERCENT)));
        if (m_callback) {
            m_callback(m_context, m_value);
        }
    }
    void Slider::handleClickEvent(const touchgfx::ClickEvent& e)
    {
        m_pressed = e.getType() == touchgfx::ClickEvent::PRESSED;
        if (m_pressed) {
            input(e.getX());
        }
    }
    void Slider::handleDragEvent(const touchgfx::DragEvent& e)
    {
        if (m_pressed) {
            input(e.getNewX());
        }
    }
}
