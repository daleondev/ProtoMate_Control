#pragma once
#include <array>
#include <touchgfx/containers/Container.hpp>
#include <touchgfx/events/ClickEvent.hpp>
#include <touchgfx/events/DragEvent.hpp>
#include <touchgfx/widgets/Box.hpp>
#include <touchgfx/widgets/TextAreaWithWildcard.hpp>
namespace ui::gui
{
    class Button final : public touchgfx::Container
    {
      public:
        using Callback = void (*)(void*);
        Button();
        void configure(const char* text, Callback callback, void* context);
        void enabled(bool value);
        void focused(bool value);
        void handleClickEvent(const touchgfx::ClickEvent& event) override;

      private:
        void update();
        touchgfx::Box m_background, m_focus;
        touchgfx::TextAreaWithOneWildcard m_label;
        std::array<touchgfx::Unicode::UnicodeChar, 40> m_text{};
        Callback m_callback{};
        void* m_context{};
        bool m_enabled{ true }, m_pressed{}, m_focused{};
    };
    class Slider final : public touchgfx::Container
    {
      public:
        using Callback = void (*)(void*, unsigned);
        Slider();
        void configure(Callback callback, void* context);
        void value(unsigned value);
        void enabled(bool value);
        void focused(bool value);
        void handleClickEvent(const touchgfx::ClickEvent& event) override;
        void handleDragEvent(const touchgfx::DragEvent& event) override;

      private:
        void position(int x);
        void update();
        touchgfx::Box m_track, m_fill, m_thumb;
        Callback m_callback{};
        void* m_context{};
        bool m_dragging{}, m_enabled{ true }, m_focused{};
    };
}
