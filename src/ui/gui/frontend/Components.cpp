#include "Components.hpp"
#include "Theme.hpp"
#include <algorithm>
#include <texts/TextKeysAndLanguages.hpp>
namespace ui::gui
{
    Button::Button()
    {
        setWidth(260);
        setHeight(theme::TOUCH_TARGET);
        setTouchable(true);
        m_background.setPosition(0, 0, 260, theme::TOUCH_TARGET);
        m_focus.setPosition(0, theme::TOUCH_TARGET - 3, 260, 3);
        m_focus.setColor(theme::TEXT);
        m_label.setPosition(16, 12, 228, 36);
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
        m_background.setColor(!m_enabled ? theme::DISABLED : m_pressed ? theme::PRESSED : theme::ACCENT);
        m_focus.setVisible(m_focused);
        invalidate();
    }
    void Button::handleClickEvent(const touchgfx::ClickEvent& event)
    {
        if (!m_enabled) {
            return;
        }
        const bool activate = m_pressed && event.getType() == touchgfx::ClickEvent::RELEASED &&
                              event.getX() >= 0 && event.getY() >= 0 && event.getX() < getWidth() &&
                              event.getY() < getHeight();
        m_pressed = event.getType() == touchgfx::ClickEvent::PRESSED;
        update();
        if (activate && m_callback) {
            m_callback(m_context);
        }
    }
    Slider::Slider()
    {
        setWidth(400);
        setHeight(64);
        setTouchable(true);
        m_track.setPosition(8, 28, 384, 8);
        m_track.setColor(theme::BORDER);
        m_fill.setPosition(8, 28, 160, 8);
        m_fill.setColor(theme::ACCENT);
        m_thumb.setPosition(160, 12, 16, 40);
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
        const auto x = 8 + int(std::min(value, 100u) * 384 / 100);
        m_fill.setWidth(x - 8);
        m_thumb.setX(x - 8);
        invalidate();
    }
    void Slider::position(int x)
    {
        const auto selected = unsigned(std::clamp(x - 8, 0, 384) * 100 / 384);
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
        m_fill.setColor(!m_enabled ? theme::DISABLED : m_dragging ? theme::PRESSED : theme::ACCENT);
        m_thumb.setColor(!m_enabled ? theme::DISABLED : m_focused ? theme::ACCENT : theme::TEXT);
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
