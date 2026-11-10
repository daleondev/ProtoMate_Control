#include "View.hpp"
#include "NumericField.hpp"
#include "Text.hpp"
#include "profiling/Trace.hpp"
#include "ui/gui/frontend/Theme.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>
namespace ui::gui::hmi
{
    namespace
    {
        constexpr unsigned NONZERO_DIGITS{ 9 }, FIELD_TEXT_BYTES{ 80 }, HEALTH_TEXT_BYTES{ 96 },
          HINT_TEXT_BYTES{ 128 };
        constexpr auto PRESENTATION_PERIOD{ std::chrono::milliseconds{ 50 } };
        constexpr std::array TITLES{ "ProtoMate",    "Axis control", "Robot motion", "Cartesian control",
                                     "Path preview", "Raw motors",   "Programs",     "Position presets",
                                     "I/O monitor",  "Settings",     "Motion jobs",  "Alarms & messages" };
    }
    View::View(Model& model, scene3d::RenderService& renderer)
      : m_model{ model }
      , m_viewport{ renderer }
    {
        layout::position(m_background, layout::shell::CANVAS);
        m_background.setColor(theme::BACKGROUND);
        add(m_background);
        layout::position(m_panels[0], layout::shell::NAVIGATION);
        layout::position(m_panels[1], layout::shell::HEADER);
        layout::position(m_panels[2], layout::shell::CONTENT);
        m_panels[2].color = theme::BACKGROUND;
        layout::position(m_panels[3], layout::shell::FOOTER);
        for (auto& p : m_panels) {
            add(p);
        }
        m_viewport.setXY(static_cast<std::int16_t>(theme::VIEW_X), static_cast<std::int16_t>(theme::VIEW_Y));
        add(m_viewport);
        m_viewport.selection(
          [](void* p, unsigned axis) { static_cast<View*>(p)->presenter->act(Axis1 + axis, true); }, this);
        add(m_axisImage);
        m_axisImage.setVisible(false);
        add(m_table);
        m_table.setVisible(false);
        add(m_brightness);
        m_brightness.setVisible(false);
        m_brightness.callback([](void* p, unsigned value) {
            auto& v{ *static_cast<View*>(p) };
            v.m_model.values[fields::BRIGHTNESS] = value;
            v.presenter->pending().brightness_changed = true;
            v.presenter->pending().changed = true;
        }, this);
        constexpr std::array names{ "Home", "Move", "Programs", "I/O", "Alarms", "Settings", "Jobs", "STOP" };
        constexpr std::array ids{ Home, Move, Programs, IO, Alarms, Settings, Jobs, GlobalStop };
        for (unsigned i{ 0 }; i < m_shell.size(); ++i) {
            auto& c{ m_shell[i] };
            c.configure(names[i], ids[i], [](void* p, Action id, bool down) {
                static_cast<View*>(p)->presenter->act(id, down);
            }, this);
            if (i < layout::shell::NAV_COUNT) {
                const auto bounds{ layout::shell::NAV_BUTTONS[i] };
                c.layout(bounds.x, bounds.y, bounds.width, bounds.height);
                c.icon(i);
            }
            else {
                const auto bounds{ i == layout::shell::JOBS_INDEX ? layout::shell::JOBS
                                                                  : layout::shell::STOP };
                c.layout(bounds.x, bounds.y, bounds.width, bounds.height);
            }
            c.danger(i == layout::shell::STOP_INDEX);
            add(c);
        }
        layout::position(m_title, layout::shell::TITLE);
        m_title.large(true);
        add(m_title);
        layout::position(m_health, layout::shell::HEALTH);
        m_health.align(touchgfx::RIGHT);
        add(m_health);
        layout::position(m_notice, layout::shell::NOTICE);
        add(m_notice);
        for (auto& c : m_controls) {
            add(c);
        }
        for (auto& l : m_labels) {
            add(l);
        }
        for (auto& lamp : m_lamps) {
            lamp.radius = layout::shell::LAMP_RADIUS;
            lamp.setVisible(false);
            add(lamp);
        }
        constexpr std::array colors{
            0xffffff, 0xffff00, 0x00ffff, 0x00ff00, 0xff00ff, 0xff0000, 0x0000ff, 0
        };
        for (unsigned i{ 0 }; i < m_bars.size(); ++i) {
            layout::position(m_bars[i], layout::shell::COLOR_BARS[i]);
            m_bars[i].setColor(colors[i]);
            m_bars[i].setVisible(false);
            add(m_bars[i]);
        }
    }
    void View::setupScreen() { build(); }
    void View::tearDownScreen()
    {
        if (m_model.backend) {
            m_model.backend->release();
        }
        m_viewport.close();
        m_visible = false;
    }
    Control& View::button(Action id,
                          const char* text,
                          int x,
                          int y,
                          int width,
                          int height,
                          bool enabled,
                          bool selected,
                          std::uint32_t tint,
                          unsigned radius,
                          bool large)
    {
        if (m_controlCount == m_controls.size()) {
            throw std::logic_error("HMI control capacity");
        }
        auto& c{ m_controls[m_controlCount++] };
        const bool appeared{ !c.isVisible() };
        c.setVisible(true);
        c.layout(x, y, width, height);
        c.held(id == JogMinus || id == JogPlus ||
               (id >= XMinus && id <= ZPlus && m_model.cartesian_jog_mode == 2));
        c.appearance({ .color = tint, .radius = radius, .large = large });
        c.configure(text, id, [](void* p, Action action, bool down) {
            auto& self{ *static_cast<View*>(p) };
            if (action == JogMinus || action == JogPlus || (action >= XMinus && action <= ZPlus)) {
                for (unsigned i{ 0 }; i < self.m_controlCount; ++i) {
                    // The held controls are found by their actual current layout below.
                    auto& control{ self.m_controls[i] };
                    if (control.isVisible() && control.action() == action) {
                        auto r{ control.getAbsoluteRect() };
                        self.presenter->pending().hold_x = r.x;
                        self.presenter->pending().hold_y = r.y;
                        self.presenter->pending().hold_width = r.width;
                        self.presenter->pending().hold_height = r.height;
                        break;
                    }
                }
            }
            self.presenter->act(action, down);
        }, this);
        c.enabled(enabled);
        c.selected(selected);
        c.danger(id == Abort);
        if (appeared) {
            c.invalidate();
        }
        return c;
    }
    void View::control(Action id, const char* text, int x, int y, int w, bool enabled, bool selected)
    {
        button(id, text, x, y, w, layout::shell::CONTROL_HEIGHT, enabled, selected);
    }
    void View::label(const char* text,
                     int x,
                     int y,
                     int w,
                     int h,
                     bool large,
                     touchgfx::Alignment alignment,
                     Typography size)
    {
        if (m_labelCount == m_labels.size()) {
            throw std::logic_error("HMI label capacity");
        }
        auto& l{ m_labels[m_labelCount++] };
        const bool appeared{ !l.isVisible() };
        if (l.getX() != x || l.getY() != y || l.getWidth() != w || l.getHeight() != h) {
            l.invalidate();
            l.setPosition(static_cast<std::int16_t>(x),
                          static_cast<std::int16_t>(y),
                          static_cast<std::int16_t>(w),
                          static_cast<std::int16_t>(h));
            l.invalidate();
        }
        l.style(large ? Typography::Body : size, alignment);
        l.setColor(theme::TEXT);
        l.text(text);
        l.setVisible(true);
        if (appeared) {
            l.invalidate();
        }
    }
    void View::value(const char* text, int x, int y, int w, bool large)
    {
        label(text,
              x,
              y,
              w,
              large ? layout::shell::LARGE_VALUE_HEIGHT : layout::shell::VALUE_HEIGHT,
              large,
              touchgfx::RIGHT);
    }
    void View::field(unsigned i, const char* name, int x, int y, int w)
    {
        Text<FIELD_TEXT_BYTES> t{};
        format(t, "{}\n{:.5g}", name, m_model.values[i]);
        control(Field0 + i, t.data(), x, y, w);
    }
    void View::lamp(int x, int y, std::uint32_t color)
    {
        auto& lamp{ m_lamps.at(m_lampCount++) };
        const bool changed{ !lamp.isVisible() || lamp.color != color || lamp.getX() != x ||
                            lamp.getY() != y };
        lamp.setPosition(static_cast<std::int16_t>(x),
                         static_cast<std::int16_t>(y),
                         layout::shell::LAMP_SIZE,
                         layout::shell::LAMP_SIZE);
        lamp.color = color;
        lamp.border = color;
        lamp.setVisible(true);
        if (changed) {
            lamp.invalidate();
        }
    }
    void View::card(unsigned index, layout::Bounds bounds)
    {
        auto& panel{ m_panels[layout::shell::PANEL_COUNT + index] };
        layout::position(panel, bounds);
        panel.setVisible(true);
    }
    void View::build(bool invalidate_page)
    {
        profiling::Scope measurement{ PROFILE_SITE_GUI_UPDATE, PROFILE_CAT_GRAPHICS };
        m_controlCount = m_labelCount = m_lampCount = 0;

        for (unsigned i{ layout::shell::PANEL_COUNT }; i < m_panels.size(); ++i) {
            m_panels[i].setVisible(false);
        }
        m_table.setVisible(false);
        m_brightness.setVisible(false);
        m_axisImage.setVisible(false);
        layout::position(m_title, layout::shell::TITLE);
        m_title.text(TITLES[unsigned(m_model.page)]);
        const bool home{ m_model.page == Page::Home && !m_model.keypad && !m_model.confirmation &&
                         !m_diagnostics };
        if (home != m_visible) {
            m_gestures.cancel();
            if (home) {
                m_viewport.open();
            }
            else {
                m_viewport.close();
            }
            m_visible = home;
        }
        if (m_model.keypad || m_model.confirmation) {
            dialog();
        }
        else if (m_model.page == Page::Home) {
            homePage();
        }
        else if (m_model.page >= Page::Axes && m_model.page <= Page::Motors) {
            movePage();
        }
        else {
            maintenancePage();
        }
        for (unsigned i{ m_controlCount }; i < m_controls.size(); ++i) {
            if (m_controls[i].isVisible()) {
                m_controls[i].invalidate();
                m_controls[i].setVisible(false);
            }
        }
        for (unsigned i{ m_labelCount }; i < m_labels.size(); ++i) {
            if (m_labels[i].isVisible()) {
                m_labels[i].invalidate();
                m_labels[i].setVisible(false);
            }
        }
        for (unsigned i{ m_lampCount }; i < m_lamps.size(); ++i) {
            if (m_lamps[i].isVisible()) {
                m_lamps[i].invalidate();
                m_lamps[i].setVisible(false);
            }
        }
        if (invalidate_page) {
            invalidate();
        }
    }
    void View::dialog()
    {
        if (m_model.confirmation) {
            label("Replace active raw-motor motion?", layout::dialog::CONFIRM_TITLE, true);
            label("This aborts the current job. It does not queue an axis move.",
                  layout::dialog::CONFIRM_HELP);
            control(Reject, "Keep current motion", layout::dialog::REJECT);
            control(Confirm, "Replace motion", layout::dialog::CONFIRM);
            return;
        }
        const auto rules{ NumericField::forModel(m_model) };
        label(rules.name, layout::dialog::FIELD_NAME);
        label(m_model.number.data(), layout::dialog::VALUE, true, touchgfx::CENTER);
        for (unsigned i{ 0 }; i < NONZERO_DIGITS; ++i) {
            const std::array<char, 2> n{ char('1' + i), 0 };
            control(Key0 + i + 1, n.data(), layout::dialog::DIGITS[i]);
        }
        control(KeySign, "+ / -", layout::dialog::SIGN, rules.signedValue());
        control(Key0, "0", layout::dialog::ZERO);
        control(KeyDot, ".", layout::dialog::DOT, !std::strchr(m_model.number.data(), '.'));
        control(KeyBack, "Delete", layout::dialog::DELETE);
        control(KeyCancel, "Cancel", layout::dialog::CANCEL);
        control(KeyApply, "Apply", layout::dialog::APPLY, rules.parse(m_model.number.data()).has_value());
        Text<HINT_TEXT_BYTES> hint{};
        rules.describe(hint);
        label(hint.data(), layout::dialog::HELP);
    }
    void View::refresh()
    {
        const auto& s{ m_model.state };
        constexpr std::array pages{ Page::Home, Page::Axes,   Page::Programs,
                                    Page::IO,   Page::Alarms, Page::Settings };
        for (unsigned i{ 0 }; i < std::size(pages); ++i) {
            m_shell[i].selected(i == 1 ? m_model.page >= Page::Axes && m_model.page <= Page::Motors
                                       : m_model.page == pages[i]);
        }
        auto health_color{ theme::GREEN };
        if (!s.fresh) {
            health_color = theme::AMBER;
        }
        if (s.failed) {
            health_color = theme::RED;
        }
        m_health.setColor(health_color);
        Text<HEALTH_TEXT_BYTES> text{};
        constexpr std::array states{ "Starting", "Ready", "Stopping", "Stopped", "Fault" };
        format(text,
               "{}  ·  {}",
               s.simulation ? "SIMULATION" : "GIGA",
               states[std::min<std::size_t>(s.combined_state, states.size() - 1)]);
        const char* health{ "Awaiting IO connection" };
        if (s.connected) {
            health = s.fresh ? text.data() : "IO telemetry stale";
        }
        if (s.failed) {
            health = "ICC FAULT · restart required";
        }
        m_health.text(health);
        m_notice.setColor(m_model.notice[0] && s.fresh && !s.failed ? severity_color(m_model.notice_severity)
                                                                    : theme::LABEL);
        const char* notice{ s.condition.data() };
        if (!s.failed && s.fresh && m_model.notice[0]) {
            notice = m_model.notice.data();
        }
        m_notice.text(notice);
        m_viewport.annotationState(s.pose_valid && s.feedback_valid, s.fresh && !s.failed);
        if (m_model.page == Page::Home && s.pose_valid && s.fresh && !s.failed && s.feedback_valid) {
            m_viewport.pose(s.feedback, s.received_us);
            m_livePose = true;
        }
    }
    bool View::input(const hal::device::TouchReport& report, bool valid)
    {
        std::array<Gestures::Exclusion, std::tuple_size_v<decltype(m_controls)>> exclusions{};
        unsigned count{};
        for (const auto& c : m_controls) {
            if (c.isVisible()) {
                const auto r{ c.getAbsoluteRect() };
                exclusions[count++] = { .x = r.x, .y = r.y, .width = r.width, .height = r.height };
            }
        }
        const auto gesture{ m_gestures.update(report, valid, m_visible, { exclusions.data(), count }) };

        if (gesture.scale != 1) {
            m_viewport.zoom(gesture.scale);
        }
        return gesture.captured && (m_visible || !valid);
    }
    void View::handleTickEvent()
    {
        const auto now{ std::chrono::steady_clock::now() };
        if (now >= m_next) {
            presenter->tick();
            m_next = now + PRESENTATION_PERIOD;
            build(false);
            refresh();
        }
        if (presenter->pending().camera_reset) {
            m_viewport.reset();
            presenter->pending().camera_reset = false;
        }
        if (presenter->pending().changed) {
            build();
            presenter->pending().changed = false;
            refresh();
        }
        if (m_visible) {
            m_viewport.tick();
        }
    }
    void View::diagnostics(bool enabled)
    {
        if (m_model.backend) {
            m_model.backend->release();
        }
        m_viewport.qualification(0);
        m_diagnostics = enabled;
        m_benchmark = false;
        m_viewport.benchmark(0);
        for (auto& b : m_bars) {
            b.setVisible(enabled);
        }
        m_model.page = Page::Home;
        build();
    }
    void View::benchmark(unsigned triangles)
    {
        diagnostics(false);
        m_benchmark = true;
        m_viewport.benchmark(triangles);
    }
    bool View::brightnessChanged() { return std::exchange(presenter->pending().brightness_changed, false); }
}
