#include "View.hpp"
#include "NumericField.hpp"
#include "profiling/Trace.hpp"
#include "ui/gui/frontend/Theme.hpp"
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <utility>
namespace ui::gui::hmi
{
    namespace
    {
        constexpr const char* titles[]{ "ProtoMate",    "Axis control", "Robot motion", "Cartesian control",
                                        "Path preview", "Raw motors",   "Programs",     "Position presets",
                                        "I/O monitor",  "Settings",     "Motion jobs",  "Alarms & messages" };
    }
    View::View(Model& model, scene3d::RenderService& renderer)
      : m_model{ model }
      , m_viewport{ renderer }
    {
        m_background.setPosition(0, 0, 1024, 600);
        m_background.setColor(theme::BACKGROUND);
        add(m_background);
        m_panels[0].setPosition(8, 8, 88, 584);
        m_panels[1].setPosition(104, 8, 912, 64);
        m_panels[2].setPosition(104, 80, 912, 444);
        m_panels[2].color = theme::BACKGROUND;
        m_panels[3].setPosition(104, 532, 912, 60);
        for (auto& p : m_panels)
            add(p);
        m_viewport.setXY(theme::VIEW_X, theme::VIEW_Y);
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
            auto& v = *static_cast<View*>(p);
            v.m_model.values[11] = value;
            v.presenter->brightnessChanged = true;
            v.presenter->changed = true;
        }, this);
        constexpr const char* names[]{
            "Home", "Move", "Programs", "I/O", "Alarms", "Settings", "Jobs", "STOP"
        };
        constexpr unsigned ids[]{ Home, Move, Programs, IO, Alarms, Settings, Jobs, GlobalStop };
        for (unsigned i = 0; i < m_shell.size(); ++i) {
            auto& c = m_shell[i];
            c.configure(names[i], ids[i], [](void* p, unsigned id, bool down) {
                static_cast<View*>(p)->presenter->act(id, down);
            }, this);
            if (i < 6) {
                c.layout(12, 12 + int(i) * 96, 80, 92);
                c.icon(i);
            }
            else
                c.layout(i == 6 ? 112 : 880, 534, i == 7 ? 128 : 104, 56);
            c.danger(i == 7);
            add(c);
        }
        m_title.setPosition(124, 25, 490, 40);
        m_title.large(true);
        add(m_title);
        m_health.setPosition(622, 28, 374, 28);
        m_health.align(touchgfx::RIGHT);
        add(m_health);
        m_notice.setPosition(232, 545, 632, 38);
        add(m_notice);
        for (auto& c : m_controls)
            add(c);
        for (auto& l : m_labels)
            add(l);
        for (auto& lamp : m_lamps) {
            lamp.radius = 6;
            lamp.setVisible(false);
            add(lamp);
        }
        constexpr std::uint32_t colors[]{ 0xffffff, 0xffff00, 0x00ffff, 0x00ff00,
                                          0xff00ff, 0xff0000, 0x0000ff, 0 };
        for (unsigned i = 0; i < 8; ++i) {
            m_bars[i].setPosition(i * 128, 0, 128, 528);
            m_bars[i].setColor(colors[i]);
            m_bars[i].setVisible(false);
            add(m_bars[i]);
        }
    }
    void View::setupScreen() { build(); }
    void View::tearDownScreen()
    {
        if (m_model.backend)
            m_model.backend->release();
        m_viewport.close();
        m_visible = false;
    }
    Control& View::button(unsigned id,
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
        if (m_controlCount == m_controls.size())
            throw std::logic_error("HMI control capacity");
        auto& c = m_controls[m_controlCount++];
        const bool appeared = !c.isVisible();
        c.setVisible(true);
        c.layout(x, y, width, height);
        c.held(id == JogMinus || id == JogPlus ||
               (id >= XMinus && id <= ZPlus && m_model.cartesianJogMode == 2));
        c.appearance(tint, radius, large);
        c.configure(text, id, [](void* p, unsigned action, bool down) {
            auto& self = *static_cast<View*>(p);
            if (action == JogMinus || action == JogPlus || (action >= XMinus && action <= ZPlus))
                for (unsigned i = 0; i < self.m_controlCount; ++i) {
                    // The held controls are found by their actual current layout below.
                    auto& control = self.m_controls[i];
                    if (control.isVisible() && control.action() == action) {
                        auto r = control.getAbsoluteRect();
                        self.presenter->holdX = r.x;
                        self.presenter->holdY = r.y;
                        self.presenter->holdWidth = r.width;
                        self.presenter->holdHeight = r.height;
                        break;
                    }
                }
            self.presenter->act(action, down);
        }, this);
        c.enabled(enabled);
        c.selected(selected);
        c.danger(id == Abort);
        if (appeared)
            c.invalidate();
        return c;
    }
    void View::control(unsigned id, const char* text, int x, int y, int w, bool enabled, bool selected)
    {
        button(id, text, x, y, w, 56, enabled, selected);
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
        if (m_labelCount == m_labels.size())
            throw std::logic_error("HMI label capacity");
        auto& l = m_labels[m_labelCount++];
        const bool appeared = !l.isVisible();
        if (l.getX() != x || l.getY() != y || l.getWidth() != w || l.getHeight() != h) {
            l.invalidate();
            l.setPosition(x, y, w, h);
            l.invalidate();
        }
        l.style(large ? Typography::Body : size, alignment);
        l.setColor(theme::TEXT);
        l.text(text);
        l.setVisible(true);
        if (appeared)
            l.invalidate();
    }
    void View::value(const char* text, int x, int y, int w, bool large)
    {
        label(text, x, y, w, large ? 44 : 28, large, touchgfx::RIGHT);
    }
    void View::field(unsigned i, const char* name, int x, int y, int w)
    {
        char t[80];
        std::snprintf(t, sizeof(t), "%s\n%.5g", name, m_model.values[i]);
        control(Field0 + i, t, x, y, w);
    }
    void View::lamp(int x, int y, std::uint32_t color)
    {
        auto& lamp = m_lamps.at(m_lampCount++);
        const bool changed = !lamp.isVisible() || lamp.color != color || lamp.getX() != x || lamp.getY() != y;
        lamp.setPosition(x, y, 12, 12);
        lamp.color = color;
        lamp.border = color;
        lamp.setVisible(true);
        if (changed)
            lamp.invalidate();
    }
    void View::card(unsigned index, int x, int y, int w, int h)
    {
        auto& p = m_panels[4 + index];
        p.setPosition(x, y, w, h);
        p.setVisible(true);
    }
    void View::build(bool invalidatePage)
    {
        profiling::Scope measurement{ PROFILE_SITE_GUI_UPDATE, PROFILE_CAT_GRAPHICS };
        m_controlCount = m_labelCount = m_lampCount = 0;

        for (unsigned i = 4; i < m_panels.size(); ++i)
            m_panels[i].setVisible(false);
        m_table.setVisible(false);
        m_brightness.setVisible(false);
        m_axisImage.setVisible(false);
        m_title.setPosition(124, 25, 490, 40);
        m_title.text(titles[unsigned(m_model.page)]);
        const bool home =
          m_model.page == Page::Home && !m_model.keypad && !m_model.confirmation && !m_diagnostics;
        if (home != m_visible) {
            m_gestures.cancel();
            if (home)
                m_viewport.open();
            else
                m_viewport.close();
            m_visible = home;
        }
        if (m_model.keypad || m_model.confirmation)
            dialog();
        else if (m_model.page == Page::Home)
            homePage();
        else if (m_model.page >= Page::Axes && m_model.page <= Page::Motors)
            movePage();
        else
            maintenancePage();
        for (unsigned i = m_controlCount; i < m_controls.size(); ++i)
            if (m_controls[i].isVisible()) {
                m_controls[i].invalidate();
                m_controls[i].setVisible(false);
            }
        for (unsigned i = m_labelCount; i < m_labels.size(); ++i)
            if (m_labels[i].isVisible()) {
                m_labels[i].invalidate();
                m_labels[i].setVisible(false);
            }
        for (unsigned i = m_lampCount; i < m_lamps.size(); ++i)
            if (m_lamps[i].isVisible()) {
                m_lamps[i].invalidate();
                m_lamps[i].setVisible(false);
            }
        if (invalidatePage)
            invalidate();
    }
    void View::dialog()
    {
        if (m_model.confirmation) {
            label("Replace active raw-motor motion?", 160, 160, 800, 44, true);
            label("This aborts the current job. It does not queue an axis move.", 160, 232, 800);
            control(Reject, "Keep current motion", 240, 352, 280);
            control(Confirm, "Replace motion", 552, 352, 240);
            return;
        }
        const auto rules = NumericField::forModel(m_model);
        label(rules.name, 160, 98, 800);
        label(m_model.number.data(), 200, 145, 660, 44, true, touchgfx::CENTER);
        for (unsigned i = 0; i < 9; ++i) {
            char n[]{ char('1' + i), 0 };
            control(Key0 + i + 1, n, 200 + int(i % 3) * 136, 210 + int(i / 3) * 64, 120);
        }
        control(KeySign, "+ / -", 200, 402, 120, rules.signedValue());
        control(Key0, "0", 336, 402, 120);
        control(KeyDot, ".", 472, 402, 120, !std::strchr(m_model.number.data(), '.'));
        control(KeyBack, "Delete", 660, 210, 200);
        control(KeyCancel, "Cancel", 660, 338, 200);
        control(KeyApply, "Apply", 660, 402, 200, rules.parse(m_model.number.data()).has_value());
        char hint[128]{};
        rules.describe(hint);
        label(hint, 160, 480, 820);
    }
    void View::refresh()
    {
        const auto& s = m_model.state;
        constexpr Page pages[]{
            Page::Home, Page::Axes, Page::Programs, Page::IO, Page::Alarms, Page::Settings
        };
        for (unsigned i = 0; i < std::size(pages); ++i)
            m_shell[i].selected(i == 1 ? m_model.page >= Page::Axes && m_model.page <= Page::Motors
                                       : m_model.page == pages[i]);
        m_health.setColor(s.failed ? theme::RED : !s.fresh ? theme::AMBER : theme::GREEN);
        char text[96];
        constexpr const char* states[]{ "Starting", "Ready", "Stopping", "Stopped", "Fault" };
        std::snprintf(text,
                      sizeof(text),
                      "%s  ·  %s",
                      s.simulation ? "SIMULATION" : "GIGA",
                      states[std::min(s.combinedState, 4U)]);
        m_health.text(s.failed                  ? "ICC FAULT · restart required"
                      : s.connected && !s.fresh ? "IO telemetry stale"
                      : s.connected             ? text
                                                : "Awaiting IO connection");
        m_notice.setColor(m_model.notice[0] && s.fresh && !s.failed ? severityColor(m_model.noticeSeverity)
                                                                    : theme::LABEL);
        m_notice.text(s.failed || !s.fresh ? s.condition.data()
                      : m_model.notice[0]  ? m_model.notice.data()
                                           : s.condition.data());
        m_viewport.annotationState(s.poseValid && s.feedbackValid, s.fresh && !s.failed);
        if (m_model.page == Page::Home && s.poseValid && s.fresh && !s.failed && s.feedbackValid) {
            m_viewport.pose(s.feedback, s.receivedUs);
            m_livePose = true;
        }
    }
    bool View::input(const hal::device::TouchReport& report, bool valid)
    {
        std::array<Gestures::Exclusion, 24> exclusions{};
        unsigned count{};
        for (const auto& c : m_controls)
            if (c.isVisible()) {
                const auto r = c.getAbsoluteRect();
                exclusions[count++] = { r.x, r.y, r.width, r.height };
            }
        const auto gesture = m_gestures.update(report, valid, m_visible, { exclusions.data(), count });

        if (gesture.scale != 1)
            m_viewport.zoom(gesture.scale);
        return gesture.captured && (m_visible || !valid);
    }
    void View::handleTickEvent()
    {
        const auto now = std::chrono::steady_clock::now();
        if (now >= m_next) {
            presenter->tick();
            m_next = now + std::chrono::milliseconds{ 50 };
            build(false);
            refresh();
        }
        if (presenter->cameraReset) {
            m_viewport.reset();
            presenter->cameraReset = false;
        }
        if (presenter->changed) {
            build();
            presenter->changed = false;
            refresh();
        }
        if (m_visible)
            m_viewport.tick();
    }
    void View::diagnostics(bool enabled)
    {
        if (m_model.backend)
            m_model.backend->release();
        m_viewport.qualification(0);
        m_diagnostics = enabled;
        m_benchmark = false;
        m_viewport.benchmark(0);
        for (auto& b : m_bars)
            b.setVisible(enabled);
        m_model.page = Page::Home;
        build();
    }
    void View::benchmark(unsigned triangles)
    {
        diagnostics(false);
        m_benchmark = true;
        m_viewport.benchmark(triangles);
    }
    bool View::brightnessChanged() { return std::exchange(presenter->brightnessChanged, false); }
}
