#include "Components.hpp"
#include "Theme.hpp"
#include <algorithm>
#include <texts/TextKeysAndLanguages.hpp>
namespace ui::gui
{
    namespace
    {
        constexpr int BUTTON_WIDTH{ 260 }, BUTTON_TEXT_X{ 16 }, BUTTON_TEXT_Y{ 12 }, BUTTON_TEXT_WIDTH{ 228 },
          BUTTON_TEXT_HEIGHT{ 36 };
        constexpr int SLIDER_WIDTH{ 400 }, SLIDER_HEIGHT{ 64 }, TRACK_X{ 8 }, TRACK_Y{ 28 },
          TRACK_WIDTH{ 384 }, TRACK_HEIGHT{ 8 };
        constexpr int THUMB_X{ 160 }, THUMB_Y{ 12 }, THUMB_WIDTH{ 16 }, THUMB_HEIGHT{ 40 };
        constexpr unsigned PERCENT{ 100 };
        std::uint32_t control_color(bool enabled, bool pressed)
        {
            if (!enabled) {
                return theme::DISABLED;
            }
            return pressed ? theme::PRESSED : theme::ACCENT;
        }
    }
    Button::Button()
    {
        setWidth(BUTTON_WIDTH);
        setHeight(static_cast<std::int16_t>(theme::TOUCH_TARGET));
        setTouchable(true);
        m_background.setPosition(0, 0, BUTTON_WIDTH, static_cast<std::int16_t>(theme::TOUCH_TARGET));
        m_focus.setPosition(0, static_cast<std::int16_t>(theme::TOUCH_TARGET - 3), BUTTON_WIDTH, 3);
        m_focus.setColor(theme::TEXT);
        m_label.setPosition(BUTTON_TEXT_X, BUTTON_TEXT_Y, BUTTON_TEXT_WIDTH, BUTTON_TEXT_HEIGHT);
        m_label.setTypedText(touchgfx::TypedText(T_BODYVALUE));
        m_label.setWildcard(m_text.data());
        m_label.setColor(theme::BACKGROUND);
        add(m_background);
        add(m_focus);
        add(m_label);
        update();
    }
    void Button::configure(const char* text, Callback callback, void* context)
    {
        touchgfx::Unicode::strncpy(m_text.data(), text, m_text.size() - 1);
        m_text.back() = 0;
        m_label.setTypedText(m_label.getTypedText());
        m_callback = callback;
        m_context = context;
        invalidate();
    }
    void Button::enabled(bool value)
    {
        m_enabled = value;
        m_pressed = false;
        setTouchable(value);
        update();
    }
    void Button::focused(bool value)
    {
        m_focused = value;
        update();
    }
    void Button::update()
    {
        m_background.setColor(control_color(m_enabled, m_pressed));
        m_focus.setVisible(m_focused);
        invalidate();
    }
    void Button::handleClickEvent(const touchgfx::ClickEvent& event)
    {
        if (!m_enabled) {
            return;
        }
        const bool activate{ m_pressed && event.getType() == touchgfx::ClickEvent::RELEASED &&
                             event.getX() >= 0 && event.getY() >= 0 && event.getX() < getWidth() &&
                             event.getY() < getHeight() };
        m_pressed = event.getType() == touchgfx::ClickEvent::PRESSED;
        update();
        if (activate && m_callback) {
            m_callback(m_context);
        }
    }
    Slider::Slider()
    {
        setWidth(SLIDER_WIDTH);
        setHeight(SLIDER_HEIGHT);
        setTouchable(true);
        m_track.setPosition(TRACK_X, TRACK_Y, TRACK_WIDTH, TRACK_HEIGHT);
        m_track.setColor(theme::BORDER);
        m_fill.setPosition(TRACK_X, TRACK_Y, THUMB_X, TRACK_HEIGHT);
        m_fill.setColor(theme::ACCENT);
        m_thumb.setPosition(THUMB_X, THUMB_Y, THUMB_WIDTH, THUMB_HEIGHT);
        m_thumb.setColor(theme::TEXT);
        add(m_track);
        add(m_fill);
        add(m_thumb);
    }
    void Slider::configure(Callback callback, void* context)
    {
        m_callback = callback;
        m_context = context;
    }
    void Slider::value(unsigned value)
    {
        const auto x{ TRACK_X + int(std::min(value, PERCENT) * TRACK_WIDTH / PERCENT) };
        m_fill.setWidth(static_cast<std::int16_t>(x - TRACK_X));
        m_thumb.setX(static_cast<std::int16_t>(x - TRACK_X));
        invalidate();
    }
    void Slider::position(int x)
    {
        const auto selected{ unsigned(std::clamp(x - TRACK_X, 0, TRACK_WIDTH) * PERCENT / TRACK_WIDTH) };
        value(selected);
        if (m_callback) {
            m_callback(m_context, selected);
        }
    }
    void Slider::enabled(bool value)
    {
        m_enabled = value;
        m_dragging = false;
        setTouchable(value);
        update();
    }
    void Slider::focused(bool value)
    {
        m_focused = value;
        update();
    }
    void Slider::update()
    {
        m_fill.setColor(control_color(m_enabled, m_dragging));
        const auto enabled_color{ m_focused ? theme::ACCENT : theme::TEXT };
        m_thumb.setColor(m_enabled ? enabled_color : theme::DISABLED);
        invalidate();
    }
    void Slider::handleClickEvent(const touchgfx::ClickEvent& event)
    {
        if (!m_enabled) {
            return;
        }
        m_dragging = event.getType() == touchgfx::ClickEvent::PRESSED;
        update();
        if (m_dragging) {
            position(event.getX());
        }
    }
    void Slider::handleDragEvent(const touchgfx::DragEvent& event)
    {
        if (m_dragging) {
            position(event.getNewX());
        }
    }
}
