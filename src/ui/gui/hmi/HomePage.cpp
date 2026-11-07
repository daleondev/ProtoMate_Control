#include "View.hpp"
#include "ui/gui/frontend/Theme.hpp"
#include <cstdio>
namespace ui::gui::hmi
{
    void View::homePage()
    {
        const auto& s = m_model.state;
        const bool usable = s.connected && s.fresh && !s.failed;
        card(0, 752, 80, 264, 444);
        label("System status", 792, 96, 208);
        lamp(768, 104, !usable ? theme::RED : s.poseValid && s.enabled ? theme::GREEN : theme::AMBER);
        const char* condition = !usable          ? "Unavailable"
                                : !s.configured  ? "Unconfigured"
                                : !s.initialized ? "Initialize"
                                : !s.enabled     ? "Disabled"
                                : s.family       ? "Busy"
                                : !s.poseValid   ? "Unreferenced"
                                                 : "Ready";
        label(condition, 768, 126, 232, 36, true);
        m_labels[m_labelCount - 1].setColor(!usable                    ? theme::RED
                                            : s.poseValid && s.enabled ? theme::GREEN
                                                                       : theme::AMBER);
        label("Axes (joints)", 768, 170, 232);
        char text[80];
        for (unsigned i = 0; i < 3; ++i) {
            const bool valid = s.poseValid && (!m_model.feedback || s.axes[i].valid);
            const int y = 198 + int(i) * 56;
            button(Axis1 + i, "", 760, y, 248, 56, true, false, theme::SURFACE, 1);
            std::snprintf(text, sizeof(text), "Axis %u", i + 1);
            label(text, 792, y + 14, 80, 28);
            lamp(772, y + 23, !usable ? theme::MUTED : valid ? theme::GREEN : theme::AMBER);
            std::snprintf(text,
                          sizeof(text),
                          valid ? "%.1f %s" : "--",
                          m_model.feedback ? s.axes[i].feedback : s.axes[i].commanded,
                          i == 2 ? "mm" : "°");
            value(text, 872, y + 14, 124);
        }
        label(m_model.feedback ? "Estimated TCP" : "Commanded TCP", 768, 374, 232);
        const auto& tcp = m_model.feedback ? s.feedbackTcp : s.tcp;
        const bool valid = s.poseValid && (!m_model.feedback || s.feedbackValid);
        constexpr const char* coordinates[]{ "X", "Y", "Z" };
        for (unsigned i = 0; i < 3; ++i) {
            label(coordinates[i], 768, 402 + int(i) * 20, 32, 22, false, touchgfx::LEFT, Typography::Small);
            std::snprintf(text, sizeof(text), valid ? "%.1f mm" : "--", tcp[i]);
            label(text, 810, 402 + int(i) * 20, 186, 22, false, touchgfx::RIGHT, Typography::Small);
        }
        const unsigned action = !s.initialized ? Initialize : !s.enabled ? Enable : Reference;
        control(action,
                action == Initialize ? "Initialize"
                : action == Enable   ? "Enable motors"
                                     : "Reference robot",
                764,
                464,
                240,
                usable && (action != Reference || s.robotConfigured),
                true);
        control(Feedback, m_model.feedback ? "Feedback" : "Commanded", 112, 82, 168);
        if (s.enabled)
            control(Disable, "Disable", 568, 82, 168, usable);
        control(ResetView, "Reset view", 568, 460, 168);
        const char* provenance =
          !s.poseValid ? (m_livePose ? "Frozen pose · reference invalid" : "Nominal CAD · reference required")
          : !s.fresh   ? "Frozen pose · telemetry stale"
          : m_model.feedback
            ? (s.feedbackValid ? "Encoder + INDEX estimates" : "Frozen pose · feedback invalid")
            : "Commanded · not measured";
        const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
        // An unchanged pose need not be redrawn; age is relevant only when stale or
        // when the displayed image still trails a pending pose/camera request.
        if (m_viewport.sourceTimeUs() && (!s.fresh || m_viewport.posePending()))
            std::snprintf(text,
                          sizeof(text),
                          "%s · image %.1f s",
                          provenance,
                          (now - m_viewport.sourceTimeUs()) / 1000000.);
        else
            std::snprintf(text, sizeof(text), "%s", provenance);
        label(text, 120, 494, 620, 28);
    }
}
