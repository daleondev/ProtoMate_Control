#pragma once
#include "Actions.hpp"
#include "Backend.hpp"
#include "ui/gui/frontend/Theme.hpp"
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
    constexpr std::uint32_t severity_color(Severity severity)
    {
        switch (severity) {
            case Severity::Error:
                return theme::RED;
            case Severity::Warning:
                return theme::AMBER;
            default:
                return theme::ACCENT;
        }
    }
    class Panel final : public touchgfx::Widget
    {
      public:
        std::uint32_t color{ theme::SURFACE }, border{ theme::BORDER };
        unsigned radius{ theme::CONTROL_RADIUS };
        void draw(const touchgfx::Rect& area) const override;
        touchgfx::Rect getSolidRect() const override
        {
            return { 0,
                     static_cast<std::int16_t>(radius),
                     getWidth(),
                     static_cast<std::int16_t>(getHeight() - (radius * 2)) };
        }
    };
    enum class Typography : std::uint8_t
    {
        Caption,
        Body,
        Heading,
        Small
    };
    template<std::size_t Capacity>
    class BasicLabel final : public touchgfx::TextAreaWithOneWildcard
    {
      public:
        // Temporary paint labels must not enqueue invalidation while the engine
        // iterates its current damage list (badges/table cells use paint_only).
        explicit BasicLabel(bool paint_only = false);
        void text(const char* value);
        void large(bool value);
        void heading();
        void small();
        void align(touchgfx::Alignment value);
        void style(Typography size, touchgfx::Alignment alignment);
        touchgfx::Alignment getAlignment() const override { return m_alignment; }

      private:
        std::array<touchgfx::Unicode::UnicodeChar, Capacity> m_text{};
        int m_wrapWidth{ -1 };
        bool m_paintOnly{};
        void damage() const
        {
            if (!m_paintOnly) {
                invalidate();
            }
        }
        void typography();
        unsigned m_size{};
        touchgfx::Alignment m_alignment{ touchgfx::LEFT };
    };
    using Label = BasicLabel<text_capacity::LABEL>;
    using ControlLabel = BasicLabel<text_capacity::CONTROL>;
    class Icon final : public touchgfx::Widget
    {
      public:
        unsigned kind{};
        void draw(const touchgfx::Rect& /*invalidatedArea*/) const override;
        touchgfx::Rect getSolidRect() const override { return { 0, 0, 0, 0 }; }
    };
    class Control final : public touchgfx::Container
    {
      public:
        using Callback = void (*)(void*, Action, bool);
        Control();
        Action action() const { return m_id; }
        void configure(const char* label, Action id, Callback cb, void* context);
        void layout(int x, int y, int width, int height = theme::TOUCH_TARGET);
        void enabled(bool value);
        void selected(bool value);
        void danger(bool value);
        void tint(std::uint32_t color);
        struct Appearance
        {
            std::uint32_t color;
            unsigned radius;
            bool large;
        };
        void appearance(Appearance style);
        void rounded(unsigned radius);
        void held(bool value) { m_held = value; }
        void icon(unsigned kind);
        void emphasis()
        {
            m_label.heading();
            alignLabel();
        }
        void handleClickEvent(const touchgfx::ClickEvent& /*event*/) override;
        void handleDragEvent(const touchgfx::DragEvent& /*event*/) override;

      private:
        void refresh();
        std::uint32_t panelColor() const;
        std::uint32_t panelBorder() const;
        void alignLabel();
        Panel m_panel;
        ControlLabel m_label;
        Icon m_icon;
        Action m_id{};
        Callback m_callback{};
        void* m_context{};
        std::uint32_t m_tint{};
        bool m_enabled{ true }, m_selected{}, m_danger{}, m_pressed{}, m_held{};
    };
}
