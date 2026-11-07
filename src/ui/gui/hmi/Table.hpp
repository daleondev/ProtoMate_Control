#pragma once
#include "Widgets.hpp"
namespace ui::gui::hmi
{
    class Table final : public touchgfx::Widget
    {
      public:
        struct Row
        {
            std::array<char, 40> name{};
            std::array<std::array<char, 24>, 5> values{};
            std::uint32_t color{ 0xedf3f8 };
            int percent{ -1 };
            bool operator==(const Row&) const = default;
        };
        struct Data
        {
            Row heading{};
            std::array<Row, 6> rows{};
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
        void draw(const touchgfx::Rect&) const override;
        touchgfx::Rect getSolidRect() const override { return { 0, 0, getWidth(), getHeight() }; }

      private:
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
        void draw(const touchgfx::Rect&) const override;
        touchgfx::Rect getSolidRect() const override { return { 0, 0, 0, 0 }; }
        void handleClickEvent(const touchgfx::ClickEvent&) override;
        void handleDragEvent(const touchgfx::DragEvent&) override;

      private:
        void input(int x);
        unsigned m_value{};
        bool m_pressed{};
        Callback m_callback{};
        void* m_context{};
    };
}
