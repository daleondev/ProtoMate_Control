#include "View.hpp"
#include "ui/gui/frontend/Theme.hpp"
#include <algorithm>
#include <cstdio>
namespace ui::gui::hmi
{
    namespace
    {
        template<std::size_t N>
        void text(std::array<char, N>& dst, const char* src)
        {
            std::snprintf(dst.data(), dst.size(), "%s", src);
        }
        void number(std::array<char, 24>& dst, std::uint64_t value)
        {
            std::snprintf(dst.data(), dst.size(), "%llu", (unsigned long long)value);
        }
    }
    void View::maintenancePage()
    {
        const auto& s = m_model.state;
        const bool usable = s.connected && s.fresh && !s.failed;
        if (m_model.page == Page::Settings) {
            settingsPage();
            return;
        }
        if (m_model.page == Page::IO) {
            control(IODigital, "Digital I/O", 112, 84, 440, true, m_model.ioTab == 0);
            control(IODrivers, "Drivers & feedback", 564, 84, 444, true, m_model.ioTab == 1);
            if (m_model.ioTab == 0) {
                card(0, 112, 148, 440, 376);
                card(1, 564, 148, 444, 376);
                label("Inputs", 132, 168, 400, 36, true);
                label("Outputs", 584, 168, 400, 36, true);
                for (unsigned i = 0; i < 3; ++i) {
                    char t[80];
                    const auto& a = s.motor(i);
                    std::snprintf(t, sizeof(t), "M%u reference", i + 1);
                    lamp(132,
                         234 + int(i) * 80,
                         !usable       ? theme::MUTED
                         : a.reference ? theme::GREEN
                                       : 0x405565);
                    label(t, 156, 224 + int(i) * 80, 216);
                    value(!usable       ? "Unknown"
                          : a.reference ? "ACTIVE"
                                        : "Released",
                          372,
                          224 + int(i) * 80,
                          160);
                    m_labels[m_labelCount - 1].setColor(!usable       ? theme::MUTED
                                                        : a.reference ? theme::GREEN
                                                                      : theme::MUTED);
                    std::snprintf(t,
                                  sizeof(t),
                                  "Driver alarm: %s",
                                  !usable   ? "unknown"
                                  : a.fault ? "ACTIVE"
                                            : "clear");
                    label(t, 132, 254 + int(i) * 80, 380, 28);
                    m_labels[m_labelCount - 1].setColor(a.fault ? theme::RED : theme::MUTED);
                }
                lamp(584, 234, !usable ? theme::MUTED : s.enabled ? theme::GREEN : 0x405565);
                label("Enable command", 608, 224, 226);
                value(!usable ? "Unknown" : s.enabled ? "ON" : "OFF", 844, 224, 144);
                m_labels[m_labelCount - 1].setColor(usable && s.enabled ? theme::GREEN : theme::MUTED);
            }
            else if (m_model.ioTab == 1) {
                Table::Data d;
                d.columns = 4;
                d.count = 3;
                text(d.heading.name, "Motor / driver");
                text(d.heading.values[0], "Ready");
                text(d.heading.values[1], "Fault");
                text(d.heading.values[2], "Feedback");
                for (unsigned i = 0; i < 3; ++i) {
                    auto& r = d.rows[i];
                    const auto& a = s.motor(i);
                    std::snprintf(r.name.data(), r.name.size(), "M%u %s", i + 1, a.driver.data());
                    text(r.values[0], !usable ? "Unknown" : a.ready ? "Yes" : "No");
                    text(r.values[1], !usable ? "Unknown" : a.fault ? "ACTIVE" : "Clear");
                    text(r.values[2], !usable || !a.valid ? "Invalid" : a.encoder ? "Encoder" : "INDEX");
                    r.color = a.fault ? theme::RED : theme::TEXT;
                }
                m_table.setPosition(120, 156, 880, 224);
                m_table.data(d);
                m_table.setVisible(true);
                for (unsigned i = 0; i < 3; ++i) {
                    char t[12];
                    std::snprintf(t, sizeof(t), "M%u details", i + 1);
                    control(Driver0 + i, t, 124 + int(i) * 292, 390, 280, true, m_model.driverDetail == i);
                }
                const auto& a = s.motor(m_model.driverDetail);
                char t[96];
                std::snprintf(t,
                              sizeof(t),
                              "Fault code %u  ·  Warning %s  ·  %s",
                              a.faultCode,
                              !usable     ? "unknown"
                              : a.warning ? "active"
                                          : "clear",
                              a.encoder ? "Encoder feedback" : "Driver-phase estimate");
                label(t, 132, 464, 860, 48);
            }
            return;
        }
        card(0, 112, 84, 896, 440);
        if (m_model.page == Page::Jobs) {
            control(TogglePreviews, m_model.previews ? "Previews" : "Execution history", 128, 96, 292);
            control(PrevJobs, "Previous", 712, 96, 132);
            control(NextJobs, "Next", 856, 96, 136);
            for (unsigned i = 0; i < m_model.jobCount; ++i) {
                card(1 + i, 128, 160 + int(i) * 84, 864, 80);
                const auto& j = m_model.jobs[i];
                char t[96];
                std::snprintf(
                  t, sizeof(t), "#%llu  %s  ·  %.2f s", (unsigned long long)j.id, j.status.data(), j.seconds);
                label(t, 136, 168 + int(i) * 84, 680);
                std::snprintf(t,
                              sizeof(t),
                              "%.0f / %.0f / %.0f pulses · %.48s",
                              j.endpoint[0],
                              j.endpoint[1],
                              j.endpoint[2],
                              j.detail.data());
                label(t, 136, 200 + int(i) * 84, 680, 42);
                control(Cancel0 + i, "Cancel", 844, 174 + int(i) * 84, 144, !j.terminal && usable);
            }
            if (!m_model.jobCount) {
                label("No jobs yet", 160, 244, 800, 44, true);
                label("CLI and GUI share the same execution history.", 160, 306, 800);
            }
            return;
        }
        if (m_model.page == Page::Alarms) {
            label(s.condition.data(), 136, 112, 856, 76, true);
            m_labels[m_labelCount - 1].setColor(s.failed ? theme::RED : theme::AMBER);
            for (unsigned i = 0; i < std::min(m_model.messageCount, 4U); ++i) {
                card(1 + i, 128, 196 + int(i) * 58, 864, 52);
                label(m_model.messages[(m_model.messageCount - 1 - i) % 8].data(),
                      136,
                      200 + int(i) * 58,
                      856,
                      52);
            }
            control(Acknowledge, "Acknowledge messages", 128, 460, 352);
            label("Hardware faults remain active until resolved.", 500, 468, 488, 48);
            return;
        }
        label(m_model.page == Page::Programs ? "Your programs will live here" : "Position presets",
              160,
              228,
              800,
              48,
              true);
        label("Not implemented yet", 160, 296, 800);
        label("No commands are generated by this page.", 160, 342, 800);
    }
    void View::settingsPage()
    {
        control(SettingsGeneral, "General", 112, 84, 288, true, m_model.settingsTab == 0);
        control(SettingsRobot, "Robot", 416, 84, 288, true, m_model.settingsTab == 1);
        control(SettingsDiagnostics, "Diagnostics", 720, 84, 288, true, m_model.settingsTab == 2);
        card(0, 112, 148, 896, 376);
        if (m_model.settingsTab == 0) {
            label("Screen brightness", 136, 184, 500, 36, true);
            char t[32];
            std::snprintf(t, sizeof(t), "%.0f %%", m_model.values[11]);
            value(t, 804, 184, 180, true);
            m_brightness.setPosition(136, 250, 840, 56);
            m_brightness.value(unsigned(m_model.values[11]));
            m_brightness.setVisible(true);
            label("Display controls are local to the UI controller.", 136, 344, 832);
            control(Presets, "Position presets", 136, 444, 300);
            return;
        }
        if (m_model.settingsTab == 1) {
            label(m_model.state.configured ? "Configuration · read only" : "Physical calibration required",
                  136,
                  176,
                  832,
                  40,
                  true);
            char t[96];
            std::snprintf(t, sizeof(t), "Identity  %llu", (unsigned long long)m_model.state.configuration);
            label(t, 136, 228, 832);
            Table::Data d;
            d.count = 3;
            d.columns = 4;
            text(d.heading.name, "Axis / units");
            text(d.heading.values[0], "Minimum");
            text(d.heading.values[1], "Maximum");
            text(d.heading.values[2], "Speed /s");
            for (unsigned i = 0; i < 3; ++i) {
                auto& r = d.rows[i];
                const auto& a = m_model.state.axes[i];
                std::snprintf(r.name.data(), r.name.size(), "Axis %u [%s]", i + 1, i == 2 ? "mm" : "°");
                const double values[]{ a.minimum, a.maximum, a.maxSpeed };
                for (unsigned j = 0; j < 3; ++j)
                    std::snprintf(r.values[j].data(),
                                  r.values[j].size(),
                                  m_model.state.configured ? "%.2f" : "--",
                                  values[j]);
            }
            m_table.setPosition(128, 270, 864, 164);
            m_table.data(d);
            m_table.setVisible(true);
            control(Motors, "Motor defaults", 136, 444, 300);
            label("Changes require an idle motor.", 460, 460, 500);
            return;
        }
        control(DiagnosticUI, "UI", 124, 156, 92, true, !m_model.diagnosticIO);
        control(DiagnosticIO, "IO", 224, 156, 92, m_model.diagnosticTab != 2, m_model.diagnosticIO);
        control(DiagnosticHeap, "Heap", 332, 156, 168, true, m_model.diagnosticTab == 0);
        control(DiagnosticStacks, "Stacks", 508, 156, 168, true, m_model.diagnosticTab == 1);
        control(DiagnosticGraphics, "Graphics", 684, 156, 168, true, m_model.diagnosticTab == 2);
        control(DiagnosticRefresh, "Refresh", 860, 156, 136);
        Table::Data d;
        d.columns = m_model.diagnosticTab == 1 ? 6 : 2;
        char note[96]{};
        if (m_model.diagnosticTab == 2) {
            text(d.heading.name, "Graphics");
            text(d.heading.values[0], "Value");
            constexpr const char* names[]{ "Presented frames", "LTDC underruns",     "DMA errors",
                                           "3D images",        "Render [us]",        "Replaced requests",
                                           "Frame age [us]",   "Maximum render [us]" };
            for (unsigned i = m_model.diagnosticOffset; i < 8 && d.count < 6; ++i) {
                auto& row = d.rows[d.count++];
                text(row.name, names[i]);
                number(row.values[0], m_model.graphics[i]);
            }
            m_table.setPosition(124, 220, 872, 238);
            m_table.data(d);
            m_table.setVisible(true);
            label("UI graphics · live counters", 136, 472, 544);
            control(DiagnosticPrevious, "Previous", 700, 464, 140);
            control(DiagnosticNext, "Next", 852, 464, 144, m_model.diagnosticOffset + d.count < 8);
            return;
        }
        DiagnosticPage page{};
        if (m_model.backend)
            m_model.backend->diagnostics(m_model.diagnosticOffset, page);
        const bool matching = page.io == m_model.diagnosticIO && page.stacks == (m_model.diagnosticTab == 1);
        if (page.busy) {
            label("Retrieving diagnostic snapshot…", 136, 248, 832, 40, true);
            return;
        }
        if (!page.available || !matching) {
            label(page.error[0] ? page.error.data() : "No snapshot. Select Refresh.", 136, 248, 832, 72);
            return;
        }
        d.count = std::min<unsigned>(page.count, d.rows.size());
        text(d.heading.name, page.stacks ? "Thread" : "Metric");
        if (page.stacks) {
            constexpr const char* heads[]{ "Prio", "Size [B]", "Now [B]", "Peak [B]", "Peak %" };
            for (unsigned i = 0; i < 5; ++i)
                text(d.heading.values[i], heads[i]);
        }
        else
            text(d.heading.values[0], "Value");
        for (unsigned i = 0; i < d.count; ++i) {
            auto& row = d.rows[i];
            const auto& in = page.rows[i];
            text(row.name, in.name.data());
            for (unsigned j = 0; j < (page.stacks ? 4U : 1U); ++j)
                number(row.values[j], in.values[j]);
            if (page.stacks) {
                if (in.values[1]) {
                    row.percent = int(std::min<std::uint64_t>(100, in.values[3] * 100 / in.values[1]));
                    std::snprintf(row.values[4].data(), row.values[4].size(), "%d %%", row.percent);
                }
                else
                    text(row.values[4], "--");
            }
            else if ((m_model.diagnosticOffset + i == 2 || m_model.diagnosticOffset + i == 8) &&
                     page.capacity) {
                row.percent = int(std::min<std::uint64_t>(100, page.used * 100 / page.capacity));
            }
            else if (m_model.diagnosticOffset + i == 0 && page.shared)
                text(row.values[0], "Dynamic");
        }
        m_table.setPosition(124, 220, 872, 238);
        m_table.data(d);
        m_table.setVisible(true);
        std::snprintf(note,
                      sizeof(note),
                      "%s · %.1f s old · %u rows%s",
                      page.shared            ? "Shared Linux runtime"
                      : m_model.diagnosticIO ? "IO snapshot"
                                             : "UI snapshot",
                      page.ageMs / 1000.,
                      page.total,
                      page.omitted ? " (truncated)" : "");
        if (page.stacks && page.omitted)
            std::snprintf(note,
                          sizeof(note),
                          "%s · %.1f s · %u rows, %u omitted",
                          page.shared ? "Shared Linux runtime"
                          : page.io   ? "IO"
                                      : "UI",
                          page.ageMs / 1000.,
                          page.total,
                          page.omitted);
        if (!page.stacks)
            std::snprintf(note,
                          sizeof(note),
                          "%s · %s · %.1f s old",
                          page.allocator == 0   ? "newlib"
                          : page.allocator == 1 ? "glibc"
                                                : "Unknown allocator",
                          page.shared ? "Shared Linux runtime"
                          : page.io   ? "IO"
                                      : "UI",
                          page.ageMs / 1000.);
        label(note, 136, 472, 544, 44);
        control(DiagnosticPrevious, "Previous", 700, 464, 140, true);
        control(DiagnosticNext, "Next", 852, 464, 144, m_model.diagnosticOffset + page.count < page.total);
    }
}
