#include "Table.hpp"
#include "ui/gui/frontend/Theme.hpp"
#include <algorithm>
#include <touchgfx/hal/HAL.hpp>
namespace ui::gui::hmi
{
    namespace
    {
        void fill(const touchgfx::Widget& w,
                  const touchgfx::Rect& area,
                  int x,
                  int y,
                  int width,
                  int height,
                  std::uint32_t color)
        {
            touchgfx::Rect r{ int16_t(x), int16_t(y), int16_t(width), int16_t(height) };
            r &= area;
            if (!r.isEmpty()) {
                const auto a = w.getAbsoluteRect();
                r.x += a.x;
                r.y += a.y;
                touchgfx::HAL::lcd().fillRect(r, color, 255);
            }
        }
    }
    void Table::draw(const touchgfx::Rect& area) const
    {
        fill(*this, area, 0, 0, getWidth(), getHeight(), theme::SURFACE);
        // Keep header and row positions stable on a partially filled final page.
        const int rowHeight = std::min(34, getHeight() / int(m_data.count + 1));
        const int nameWidth = m_data.columns == 2 ? getWidth() / 2 : getWidth() * 26 / 100;
        const int cellWidth = (getWidth() - nameWidth) / int(std::max(1U, m_data.columns - 1));
        for (unsigned i = 0; i <= m_data.count; ++i) {
            const auto& row = i ? m_data.rows[i - 1] : m_data.heading;
            const int y = int(i) * rowHeight;
            fill(*this, area, 0, y, getWidth(), rowHeight, i == 0 ? 0x223645 : i % 2 ? 0x192a36 : 0x172630);
            fill(*this, area, 0, y + rowHeight - 1, getWidth(), 1, theme::BORDER);
            for (unsigned c = 0; c < m_data.columns; ++c) {
                ControlLabel label{ true };
                const auto origin = getAbsoluteRect();
                const int x = c ? nameWidth + int(c - 1) * cellWidth : 0;
                const int width = c ? cellWidth : nameWidth;
                label.setPosition(origin.x + x + 12, origin.y + y, width - 24, rowHeight);
                label.align(c ? touchgfx::RIGHT : touchgfx::LEFT);
                label.text(c ? row.values[c - 1].data() : row.name.data());
                label.setColor(i ? row.color : theme::MUTED);
                label.setY(origin.y + y + (rowHeight - label.getTextHeight()) / 2);
                auto clip = area;
                clip.x += origin.x;
                clip.y += origin.y;
                clip &= label.getRect();
                clip.x -= label.getX();
                clip.y -= label.getY();
                if (!clip.isEmpty())
                    label.draw(clip);
            }
            if (i && row.percent >= 0) {
                const int width = std::min(row.percent, 100) * (cellWidth - 24) / 100;
                fill(*this,
                     area,
                     getWidth() - cellWidth + 12,
                     y + rowHeight - 5,
                     width,
                     3,
                     row.percent >= 90 ? theme::AMBER : theme::ACCENT);
            }
        }
    }
    void Slider::value(unsigned v)
    {
        v = std::min(v, 100U);
        if (v != m_value) {
            m_value = v;
            invalidate();
        }
    }
    void Slider::draw(const touchgfx::Rect& area) const
    {
        const int x = 16 + int(m_value) * (getWidth() - 32) / 100;
        fill(*this, area, 16, 26, getWidth() - 32, 6, theme::BORDER);
        fill(*this, area, 16, 26, x - 16, 6, theme::ACCENT);
        fill(*this, area, x - 8, 17, 16, 24, theme::TEXT);
    }
    void Slider::input(int x)
    {
        value(unsigned(std::clamp((x - 16) * 100 / std::max(1, int(getWidth()) - 32), 0, 100)));
        if (m_callback)
            m_callback(m_context, m_value);
    }
    void Slider::handleClickEvent(const touchgfx::ClickEvent& e)
    {
        m_pressed = e.getType() == touchgfx::ClickEvent::PRESSED;
        if (m_pressed)
            input(e.getX());
    }
    void Slider::handleDragEvent(const touchgfx::DragEvent& e)
    {
        if (m_pressed)
            input(e.getNewX());
    }
}
