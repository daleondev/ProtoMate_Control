#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <touchgfx/containers/Container.hpp>
#include <touchgfx/events/ClickEvent.hpp>
#include <touchgfx/events/DragEvent.hpp>
#include <touchgfx/widgets/TextAreaWithWildcard.hpp>
#include <touchgfx/widgets/Widget.hpp>

namespace ui::gui::hmi
{
    class Panel final : public touchgfx::Widget
    {
      public:
        std::uint32_t color{ 0x192936 }, border{ 0x304352 };
        unsigned radius{ 10 };
        void draw(const touchgfx::Rect& area) const override;
        touchgfx::Rect getSolidRect() const override
        {
            return { 0,
                     static_cast<std::int16_t>(radius),
                     getWidth(),
                     static_cast<std::int16_t>(getHeight() - radius * 2) };
        }
    };
    template<std::size_t Capacity>
    class BasicLabel final : public touchgfx::TextAreaWithOneWildcard
    {
      public:
        BasicLabel();
        void text(const char* value);
        void large(bool value);

      private:
        std::array<touchgfx::Unicode::UnicodeChar, Capacity> m_text{};
    };
    using Label = BasicLabel<192>;
    using ControlLabel = BasicLabel<48>;
    class Icon final : public touchgfx::Widget
    {
      public:
        unsigned kind{};
        void draw(const touchgfx::Rect&) const override;
        touchgfx::Rect getSolidRect() const override { return { 0, 0, 0, 0 }; }
    };
    class Control final : public touchgfx::Container
    {
      public:
        using Callback = void (*)(void*, unsigned, bool);
        Control();
        void configure(const char* label, unsigned id, Callback callback, void* context);
        void layout(int x, int y, int width, int height = 56);
        void enabled(bool value);
        void selected(bool value);
        void danger(bool value);
        void held(bool value) { m_held = value; }
        void icon(unsigned kind);
        void handleClickEvent(const touchgfx::ClickEvent&) override;
        void handleDragEvent(const touchgfx::DragEvent&) override;

      private:
        void refresh();
        Panel m_panel;
        ControlLabel m_label;
        Icon m_icon;
        unsigned m_id{};
        Callback m_callback{};
        void* m_context{};
        bool m_enabled{ true }, m_selected{}, m_danger{}, m_pressed{}, m_held{};
    };
}
