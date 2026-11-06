#include "View.hpp"
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <utility>
namespace ui::gui::hmi
{
    namespace
    {
        constexpr const char* axisNames[]{ "Shoulder", "Elbow", "Z" };
        constexpr const char* pageNames[]{
            "Robot overview", "Axis control",           "Robot motion", "Cartesian point steps",
            "Path preview",   "Raw motor control",      "Programs",     "Position presets",
            "I/O monitor",    "Settings & diagnostics", "Motion jobs",  "Alarms & messages"
        };
        constexpr const char* buffers[]{ "Aborting",       "Buffered",   "Blend low",
                                         "Blend previous", "Blend next", "Blend high" };

    }
    View::View(Model& model, scene3d::RenderService& renderer)
      : m_model{ model }
      , m_viewport{ renderer }
    {
        m_background.setPosition(0, 0, 1024, 600);
        m_background.setColor(0x0c151e);
        add(m_background);
        m_panels[0].setPosition(8, 8, 112, 584);
        m_panels[0].color = 0x182631;
        m_panels[1].setPosition(124, 8, 892, 64);
        m_panels[1].color = 0x15232f;
        m_panels[2].setPosition(124, 80, 892, 440);
        m_panels[2].color = 0x101820;
        m_panels[3].setPosition(124, 528, 892, 64);
        m_panels[3].color = 0x15232f;
        for (auto& p : m_panels)
            add(p);
        m_viewport.setXY(132, 112);
        add(m_viewport);
        constexpr const char* names[]{
            "Home", "Move", "Programs", "I/O", "Settings", "Jobs", "Alarms", "STOP"
        };
        constexpr unsigned ids[]{ Home, Move, Programs, IO, Settings, Jobs, Alarms, GlobalStop };
        for (unsigned i = 0; i < m_shell.size(); ++i) {
            auto& c = m_shell[i];
            c.configure(names[i], ids[i], [](void* p, unsigned id, bool pressed) {
                auto& self = *static_cast<View*>(p);
                self.presenter->act(id, pressed);
            }, this);
            if (i < 5) {
                c.layout(12, 88 + int(i) * 84, 104, 68);
                c.icon(i);
            }
            else
                c.layout(i == 5 ? 132 : i == 6 ? 248 : 868, 532, i == 7 ? 140 : 108);
            c.danger(i == 7);
            add(c);
        }
        m_title.setPosition(144, 20, 570, 40);
        m_title.large(true);
        add(m_title);
        m_health.setPosition(720, 27, 280, 32);
        add(m_health);
        m_notice.setPosition(372, 535, 482, 52);
        add(m_notice);
        for (auto& l : m_labels)
            add(l);
        for (auto& c : m_controls)
            add(c);
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
    void View::control(unsigned id, const char* text, int x, int y, int w, bool enabled, bool selected)
    {
        if (m_controlCount == m_controls.size())
            throw std::logic_error("HMI page control capacity exceeded");
        auto& c = m_controls[m_controlCount++];
        c.setVisible(true);
        c.layout(x, y, w);
        c.held(id == JogMinus || id == JogPlus);
        c.configure(text, id, [](void* p, unsigned action, bool pressed) {
            auto& self = *static_cast<View*>(p);
            if (action == JogMinus || action == JogPlus) {
                self.presenter->holdX = action == JogMinus ? 144 : 374;
                self.presenter->holdY = 340;
                self.presenter->holdWidth = 214;
                self.presenter->holdHeight = 56;
            }
            self.presenter->act(action, pressed);
        }, this);
        c.enabled(enabled);
        c.selected(selected);
        c.danger(id == Abort);
    }
    void View::label(const char* text, int x, int y, int w, int h, bool large)
    {
        if (m_labelCount == m_labels.size())
            throw std::logic_error("HMI label capacity exceeded");
        auto& l = m_labels[m_labelCount++];
        l.setPosition(x, y, w, h);
        l.large(large);
        l.text(text);
        l.setVisible(true);
    }
    void View::field(unsigned i, const char* name, int x, int y, int w)
    {
        char text[80];
        std::snprintf(text, sizeof(text), "%s  %.5g", name, m_model.values[i]);
        control(Field0 + i, text, x, y, w);
    }
    void View::build(bool invalidatePage)
    {
        m_controlCount = m_labelCount = 0;
        for (auto& c : m_controls)
            c.setVisible(false);
        for (auto& l : m_labels)
            l.setVisible(false);
        m_title.text(pageNames[unsigned(m_model.page)]);
        const auto& s = m_model.state;
        const bool usable = s.connected && s.fresh && !s.failed;
        const bool axes = usable && s.configured && s.enabled && s.initialized;
        const bool robot = axes && s.robotConfigured;
        const bool home =
          m_model.page == Page::Home && !m_model.keypad && !m_model.confirmation && !m_diagnostics;
        if (home != m_visible) {
            if (home)
                m_viewport.open();
            else
                m_viewport.close();
            m_visible = home;
        }
        if (m_model.keypad) {
            constexpr const char* other[]{ "Speed",     "Acceleration",        "Deceleration",
                                           "Jerk",      "Timeout seconds",     "Jog seconds",
                                           "Latch rpm", "Speed factor (0..1)", "Brightness percent" };
            const char* unit =
              m_model.field < 3
                ? (m_model.page == Page::Motors ? "Shaft degrees"
                   : m_model.page == Page::Axes
                     ? (m_model.axis == 2 ? "Z millimetres" : "Axis degrees")
                     : (m_model.tab == 0 && m_model.field < 2 ? "Joint degrees" : "Position millimetres"))
                : other[m_model.field - 3];
            char title[120];
            std::snprintf(title, sizeof(title), "Edit %s · editing never starts motion", unit);
            label(title, 160, 100, 800);
            label(m_model.number.data(), 200, 150, 650, 44, true);
            for (unsigned i = 0; i < 9; ++i) {
                char n[]{ char('1' + i), 0 };
                control(Key0 + i + 1, n, 200 + int(i % 3) * 136, 210 + int(i / 3) * 64, 120);
            }
            control(KeySign, "+ / -", 200, 402, 120);
            control(Key0, "0", 336, 402, 120);
            control(KeyDot, ".", 472, 402, 120);
            control(KeyBack, "Delete", 660, 210, 200);
            control(KeyCancel, "Cancel", 660, 338, 200);
            control(KeyApply, "Apply", 660, 402, 200);
            label("Negative values allowed only for positions / raw rpm", 160, 480, 820);
            invalidate();
            return;
        }
        if (m_model.confirmation) {
            label("Replace active raw-motor motion?", 168, 160, 800, 44, true);
            label("Aborting interrupts its current job. This is not a queued axis move.", 168, 232, 800);
            control(Reject, "Keep current motion", 240, 352, 280);
            control(Confirm, "Replace motion", 552, 352, 240);
            invalidate();
            return;
        }
        if (m_model.page >= Page::Axes && m_model.page <= Page::Motors) {
            constexpr const char* names[]{ "Axes", "Robot", "XYZ steps", "Preview", "Raw motors" };
            for (unsigned i = 0; i < 5; ++i)
                control(
                  Axes + i, names[i], 144 + int(i) * 170, 92, 160, true, unsigned(m_model.page) == i + 1);
        }
        char text[192];
        switch (m_model.page) {
            case Page::Home:
                label("PROTOMATE  /  LIVE VIEW", 144, 84, 600);
                std::snprintf(text, sizeof(text), "Frozen pose · telemetry age %.1f s", s.ageMs / 1000.0);
                label(!s.poseValid       ? (m_livePose ? "Frozen pose · calibration / reference invalid"
                                                       : "Nominal CAD pose · calibration / reference required")
                      : !s.fresh         ? text
                      : m_model.feedback ? (s.feedbackValid ? "Encoder + INDEX phase estimates"
                                                            : "Frozen pose · feedback invalid")
                                         : "Commanded position · not measured",
                      144,
                      112,
                      620,
                      40);
                for (unsigned i = 0; i < 3; ++i) {
                    std::snprintf(text,
                                  sizeof(text),
                                  "%s   %.2f %s",
                                  axisNames[i],
                                  m_model.feedback ? s.axes[i].feedback : s.axes[i].commanded,
                                  i == 2 ? "mm" : "deg");
                    if (!s.poseValid || (m_model.feedback && !s.axes[i].valid))
                        std::snprintf(text, sizeof(text), "%s  --", axisNames[i]);
                    control(Shoulder + i, text, 780, 120 + int(i) * 66, 218);
                }
                label("Estimated TCP", 788, 326, 216);
                std::snprintf(text,
                              sizeof(text),
                              "X %7.1f\nY %7.1f\nZ %7.1f mm",
                              m_model.feedback ? s.feedbackTcp[0] : s.tcp[0],
                              m_model.feedback ? s.feedbackTcp[1] : s.tcp[1],
                              m_model.feedback ? s.feedbackTcp[2] : s.tcp[2]);
                label(s.poseValid && (!m_model.feedback || s.feedbackValid)
                        ? text
                        : "No valid TCP\nCalibration / reference or feedback required",
                      788,
                      358,
                      216,
                      100);
                control(Initialize, "Initialize", 144, 452, 142, usable && !s.enabled);
                control(s.enabled ? Disable : Enable,
                        s.enabled ? "Disable" : "Enable",
                        296,
                        452,
                        126,
                        usable && (s.initialized || s.enabled));
                control(Reference, "Reference robot", 432, 452, 190, robot);
                control(Feedback, m_model.feedback ? "Feedback" : "Commanded", 632, 452, 140);
                control(ZoomIn, "+", 636, 372, 60);
                control(ZoomOut, "-", 704, 372, 60);
                control(ResetView, "Reset view", 632, 308, 132);
                break;
            case Page::Axes:
                for (unsigned i = 0; i < 3; ++i)
                    control(
                      Shoulder + i, axisNames[i], 144 + int(i) * 160, 160, 148, true, m_model.axis == i);
                std::snprintf(text,
                              sizeof(text),
                              "%.2f %s  |  limits %.2f .. %.2f  |  %s",
                              s.axes[m_model.axis].commanded,
                              m_model.axis == 2 ? "mm" : "deg",
                              s.axes[m_model.axis].minimum,
                              s.axes[m_model.axis].maximum,
                              s.axes[m_model.axis].referenced ? "Referenced" : "Unreferenced");
                label(text, 144, 224, 840);
                if (m_model.tab == 1) {
                    field(3, m_model.axis == 2 ? "Speed mm/s" : "Speed deg/s", 144, 268, 260);
                    field(4, m_model.axis == 2 ? "Accel mm/s2" : "Accel deg/s2", 424, 268, 260);
                    field(5, m_model.axis == 2 ? "Decel mm/s2" : "Decel deg/s2", 704, 268, 280);
                    field(6, m_model.axis == 2 ? "Jerk mm/s3" : "Jerk deg/s3", 144, 344, 260);
                    field(7, "Timeout s", 424, 344, 260);
                    field(8, "Jog seconds", 704, 344, 280);
                    control(AxisJogPage, "Back to movement", 144, 438, 260);
                    label(
                      "0 selects configured defaults. Changes apply to subsequent moves.", 424, 438, 560, 70);
                }
                else {
                    control(Minus, "- step", 144, 268, 126, axes);
                    control(Plus, "+ step", 282, 268, 126, axes);
                    control(StepSmall, "0.1", 420, 268, 76, true, m_model.step == .1);
                    control(StepMedium, "1", 508, 268, 76, true, m_model.step == 1);
                    control(StepLarge, "10", 596, 268, 76, true, m_model.step == 10);
                    control(Reference, "Reference", 688, 268, 142, axes);
                    control(AxisStop, "Stop axis", 844, 268, 140, usable);
                    control(JogMinus, m_model.continuous ? "Hold  -" : "Jog  -", 144, 340, 214, axes);
                    control(JogPlus, m_model.continuous ? "Hold  +" : "Jog  +", 374, 340, 214, axes);
                    control(m_model.continuous ? Timed : Continuous,
                            m_model.continuous ? "Continuous (held)" : "Timed jog",
                            610,
                            340,
                            218);
                    control(AxisDynamics, "Dynamics", 840, 340, 144);
                    field(0, "Position", 144, 412, 214);
                    control(Absolute,
                            m_model.absolute ? "Absolute" : "Absolute",
                            374,
                            412,
                            160,
                            true,
                            m_model.absolute);
                    control(Relative, "Relative", 544, 412, 140, true, !m_model.absolute);
                    control(Execute, "Move", 704, 412, 280, axes);
                    std::snprintf(text,
                                  sizeof(text),
                                  "%s feedback: %.2f %s · %s",
                                  s.axes[m_model.axis].encoder ? "Encoder" : "INDEX estimate",
                                  s.axes[m_model.axis].feedback,
                                  m_model.axis == 2 ? "mm" : "deg",
                                  s.axes[m_model.axis].valid ? "valid" : "unavailable");
                    label(text, 144, 478, 840);
                }
                break;
            case Page::Robot:
            case Page::Preview:
                control(PreviewJoints, "Joint destination", 144, 160, 240, true, m_model.tab == 0);
                control(PreviewPoint, "Cartesian destination", 400, 160, 268, true, m_model.tab == 1);
                if (m_model.page == Page::Robot) {
                    control(Relative, "Relative", 688, 160, 136, true, !m_model.absolute);
                    control(Absolute, "Absolute", 836, 160, 156, true, m_model.absolute);
                }
                field(0, m_model.tab == 0 ? "Shoulder deg" : "X mm", 144, 236, 260);
                field(1, m_model.tab == 0 ? "Elbow deg" : "Y mm", 424, 236, 260);
                field(2, "Z mm", 704, 236, 280);
                field(10, "Speed factor", 144, 308, 260);
                field(4, "Accel factor (0=1)", 424, 308, 260);
                field(5, "Decel factor (0=1)", 704, 308, 280);
                field(6, "Jerk factor (0=1)", 144, 380, 260);
                field(7, "Timeout s", 424, 380, 260);
                if (m_model.page == Page::Robot) {
                    control(Execute, "Move to destination", 704, 380, 280, robot);
                    control(Reference, "Reference robot", 144, 452, 260, robot);
                    label("Joint interpolation: TCP path may curve. Factors apply to the next move.",
                          424,
                          458,
                          560,
                          52);
                }
                else {
                    control(PreviewLine, "Straight line", 688, 160, 296, true, m_model.tab == 2);
                    control(RunPreview, "Calculate preview", 704, 380, 280, usable && s.robotConfigured);
                    control(TogglePreviews, "Preview results", 144, 452, 260);
                    label("No motion. Straight-line execution unavailable.", 424, 458, 560);
                }
                break;
            case Page::Cartesian:
                label("Discrete destinations · joint interpolation can curve the TCP path", 144, 164, 840);
                for (unsigned i = 0; i < 6; ++i) {
                    constexpr const char* names[]{ "X -", "X +", "Y -", "Y +", "Z -", "Z +" };
                    control(XMinus + i, names[i], 144 + int(i % 2) * 200, 224 + int(i / 2) * 76, 180, robot);
                }
                control(StepSmall, "0.1 mm", 600, 224, 112, true, m_model.step == .1);
                control(StepMedium, "1 mm", 724, 224, 112, true, m_model.step == 1);
                control(StepLarge, "10 mm", 848, 224, 136, true, m_model.step == 10);
                field(10, "Speed factor", 600, 308, 384);
                label("Continuous Cartesian jog is not available.\nUse axis jog for held movement.",
                      600,
                      398,
                      384,
                      80);
                break;
            case Page::Motors:
                for (unsigned i = 0; i < 3; ++i) {
                    std::snprintf(text, sizeof(text), "M%u", i + 1);
                    control(Shoulder + i, text, 144 + int(i) * 120, 160, 108, true, m_model.axis == i);
                }
                control(Relative, "Relative", 528, 160, 140, true, !m_model.absolute);
                control(Absolute, "Absolute", 680, 160, 140, true, m_model.absolute);
                control(Abort, "ABORT", 832, 160, 152, usable);
                std::snprintf(text,
                              sizeof(text),
                              "Shaft %.2f deg · %.2f rpm · %s %.2f deg (%s)",
                              s.motor(m_model.axis).shaft,
                              s.motor(m_model.axis).rpm,
                              s.motor(m_model.axis).encoder ? "Encoder" : "INDEX estimate",
                              s.motor(m_model.axis).shaftFeedback,
                              s.motor(m_model.axis).valid ? "valid" : "unavailable");
                label(text, 144, 222, 840, 28);
                field(0, "Shaft degrees", 144, 254, 260);
                field(3, "Speed rpm", 424, 254, 260);
                control(NextBuffer, buffers[m_model.buffer], 704, 254, 280);
                field(4, "Accel deg/s2", 144, 322, 260);
                field(5, "Decel deg/s2", 424, 322, 260);
                field(6, "Jerk deg/s3", 704, 322, 280);
                field(7, "Timeout s", 144, 390, 260);
                field(9, "Latch rpm", 424, 390, 260);
                control(Execute, "Move motor", 704, 390, 280, usable && s.enabled);
                control(Reference, "Home motor", 144, 452, 194, usable && s.enabled);
                control(ActiveSpeed, "Set active speed", 350, 452, 214, usable);
                control(
                  ApplyDefaults, "Save defaults", 576, 452, 204, usable && !s.motor(m_model.axis).owner);
                label("Abort invalidates reference", 792, 450, 208, 60);
                break;
            case Page::Jobs:
                control(TogglePreviews, m_model.previews ? "Previews" : "Execution history", 144, 92, 254);
                control(PrevJobs, "Previous", 698, 92, 140);
                control(NextJobs, "Next", 850, 92, 140);
                for (unsigned i = 0; i < m_model.jobCount; ++i) {
                    const auto& j = m_model.jobs[i];
                    std::snprintf(text,
                                  sizeof(text),
                                  "#%llu  %s  ·  %.2f s\n%.0f / %.0f / %.0f pulses  %s",
                                  (unsigned long long)j.id,
                                  j.status.data(),
                                  j.seconds,
                                  j.endpoint[0],
                                  j.endpoint[1],
                                  j.endpoint[2],
                                  j.detail.data());
                    label(text, 152, 168 + int(i) * 84, 650, 74);
                    control(Cancel0 + i, "Cancel", 836, 168 + int(i) * 84, 148, !j.terminal && usable);
                }
                if (!m_model.jobCount)
                    label("No jobs yet. CLI and GUI share this history.", 168, 232, 800, 48, true);
                break;
            case Page::IO:
                label("Physical inputs & driver diagnostics  ·  read only", 152, 106, 820, 40, true);
                for (unsigned i = 0; i < 3; ++i) {
                    const auto& a = s.motor(i);
                    std::snprintf(
                      text,
                      sizeof(text),
                      "M%u  %s\nReference %s   Driver %s   Fault %s (%u)   Warning %s\nFeedback %s · %s",
                      i + 1,
                      a.driver.data(),
                      a.reference ? "ON" : "off",
                      a.ready ? "ready" : "not ready",
                      a.fault ? "ACTIVE" : "clear",
                      a.faultCode,
                      a.warning ? "yes" : "no",
                      a.encoder ? "encoder" : "driver phase (INDEX)",
                      a.valid ? "valid" : "invalid");
                    label(text, 152, 164 + int(i) * 100, 840, 96);
                }
                label("General-purpose / analog I/O: not implemented", 152, 476, 820);
                break;
            case Page::Settings:
                field(11, "Brightness %", 144, 104, 270);
                control(MemoryUI, "UI memory", 434, 104, 240);
                control(MemoryIO, "IO memory", 694, 104, 290, usable);
                control(MemoryUIStack, "UI stacks", 434, 170, 240);
                control(MemoryIOStack, "IO stacks", 694, 170, 290, usable);
                label(s.configured ? "Configuration (read only)" : "Physical calibration required",
                      152,
                      238,
                      832,
                      40,
                      true);
                std::snprintf(text,
                              sizeof(text),
                              "Configuration %llu\n%.150s",
                              (unsigned long long)s.configuration,
                              s.details.data());
                label(text, 152, 282, 832, 76);
                label(m_model.graphics.data(), 152, 366, 832, 64);
                control(Motors, "Motor defaults", 144, 440, 240);
                control(Presets, "Position presets", 402, 440, 260);
                label("Network / user settings: not implemented", 682, 438, 310, 64);
                break;
            case Page::Alarms:
                label(s.condition.data(), 152, 110, 832, 72, true);
                for (unsigned i = 0; i < std::min(m_model.messageCount, 4U); ++i)
                    label(m_model.messages[(m_model.messageCount - 1 - i) % 8].data(),
                          152,
                          194 + int(i) * 62,
                          832,
                          58);
                control(Acknowledge, "Acknowledge messages", 152, 452, 300);
                label("Active faults cannot be cleared here", 476, 462, 500);
                break;
            case Page::Programs:
            case Page::Presets:
                label(m_model.page == Page::Programs ? "Your programs will live here" : "Position presets",
                      200,
                      224,
                      760,
                      48,
                      true);
                label("Not implemented yet. No commands are generated by this page.", 200, 292, 760, 80);
                break;
        }
        if (invalidatePage)
            invalidate();
    }
    void View::refresh()
    {
        const auto& s = m_model.state;
        for (unsigned i = 0; i < 5; ++i)
            m_shell[i].selected(i == 0   ? m_model.page == Page::Home
                                : i == 1 ? m_model.page >= Page::Axes && m_model.page <= Page::Motors
                                : i == 2 ? m_model.page == Page::Programs
                                : i == 3 ? m_model.page == Page::IO
                                         : m_model.page == Page::Settings);
        m_health.setColor(s.failed ? 0xff7e84 : !s.fresh ? 0xe1b661 : 0x65d99b);
        char health[96];
        constexpr const char* states[]{ "Starting", "Ready", "Stopping", "Stopped", "Fault" };
        std::snprintf(health,
                      sizeof(health),
                      "%s · %s",
                      s.simulation ? "SIMULATION" : "GIGA",
                      states[std::min(s.combinedState, 4U)]);
        m_health.text(s.failed                  ? "ICC FAULT · restart required"
                      : s.connected && !s.fresh ? "IO telemetry stale"
                      : s.connected             ? health
                                                : "Awaiting IO connection");
        m_notice.text(s.failed || !s.fresh ? s.condition.data()
                      : m_model.notice[0]  ? m_model.notice.data()
                                           : s.condition.data());
        if (m_model.page == Page::Home) {
            if (s.poseValid && s.fresh && (!m_model.feedback || s.feedbackValid)) {
                m_viewport.pose(m_model.feedback ? s.feedback : s.commanded, s.receivedUs);
                m_livePose = true;
            }
            // Other readouts are rebuilt only on telemetry, never every display tick.
        }
    }
    void View::handleTickEvent()
    {
        const auto now = std::chrono::steady_clock::now();
        if (now >= m_next) {
            const auto oldSequence = m_model.state.sequence;
            const auto oldFresh = m_model.state.fresh;
            const auto oldConnected = m_model.state.connected;
            const auto oldFailed = m_model.state.failed;
            presenter->tick();
            m_next = now + std::chrono::milliseconds{ 50 };
            if (oldSequence != m_model.state.sequence || oldFresh != m_model.state.fresh ||
                oldConnected != m_model.state.connected || oldFailed != m_model.state.failed)
                build(false);
            if (m_model.page == Page::Jobs || m_model.page == Page::Settings || m_model.page == Page::Alarms)
                build(false);
            if (m_model.page == Page::Home && !m_model.state.fresh)
                build(false);
            refresh();
        }
        if (presenter->cameraIn) {
            m_viewport.zoom(.85f);
            presenter->cameraIn = false;
        }
        if (presenter->cameraOut) {
            m_viewport.zoom(1.18f);
            presenter->cameraOut = false;
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
