#pragma once
#include "Widgets.hpp"
namespace ui::gui::hmi
{
    class Table final : public touchgfx::Widget
    {
      public:
        static constexpr unsigned VALUE_COLUMNS{ 5 };
        struct Row
        {
            std::array<char, text_capacity::NAME> name{};
            std::array<std::array<char, text_capacity::NUMBER>, VALUE_COLUMNS> values{};
            std::uint32_t color{ theme::TEXT };
            int percent{ -1 };
            bool operator==(const Row&) const = default;
        };
        struct Data
        {
            Row heading{};
            std::array<Row, DIAGNOSTIC_ROWS> rows{};
            unsigned count{}, columns{ 2 };
            bool operator==(const Data&) const = default;
        };
        void data(const Data& next)
        {
            if (!(m_data == next)) {
                m_data = next;
                invalidate();
            }
        }
        void draw(const touchgfx::Rect& /*invalidatedArea*/) const override;
        touchgfx::Rect getSolidRect() const override { return { 0, 0, getWidth(), getHeight() }; }

      private:
        struct RowGeometry
        {
            int y{}, height{}, name_width{}, cell_width{};
            bool header{};
        };
        void drawRow(const touchgfx::Rect& area, const Row& row, RowGeometry geometry) const;
        Data m_data{};
    };
    class Slider final : public touchgfx::Widget
    {
      public:
        using Callback = void (*)(void*, unsigned);
        void value(unsigned value);
        void callback(Callback cb, void* context)
        {
            m_callback = cb;
            m_context = context;
            setTouchable(true);
        }
        void draw(const touchgfx::Rect& /*invalidatedArea*/) const override;
        touchgfx::Rect getSolidRect() const override { return { 0, 0, 0, 0 }; }
        void handleClickEvent(const touchgfx::ClickEvent& /*event*/) override;
        void handleDragEvent(const touchgfx::DragEvent& /*event*/) override;

      private:
        void input(int x);
        unsigned m_value{};
        bool m_pressed{};
        Callback m_callback{};
        void* m_context{};
    };
}
