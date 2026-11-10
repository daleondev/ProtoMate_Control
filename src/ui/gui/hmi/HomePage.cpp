#include "Text.hpp"
#include "View.hpp"
#include "ui/gui/frontend/Theme.hpp"
#include <array>
namespace ui::gui::hmi
{
    namespace
    {
        constexpr unsigned STATUS_TEXT_BYTES{ 80 };
        constexpr unsigned LINEAR_AXIS{ 2 };
        bool usable(const Snapshot& state) { return state.connected && state.fresh && !state.failed; }
        std::uint32_t status_color(const Snapshot& state)
        {
            if (!usable(state)) {
                return theme::RED;
            }
            return state.pose_valid && state.enabled ? theme::GREEN : theme::AMBER;
        }
        const char* readiness(const Snapshot& state)
        {
            if (!usable(state)) {
                return "Unavailable";
            }
            if (!state.configured) {
                return "Unconfigured";
            }
            if (!state.initialized) {
                return "Initialize";
            }
            if (!state.enabled) {
                return "Disabled";
            }
            if (state.family) {
                return "Busy";
            }
            return state.pose_valid ? "Ready" : "Unreferenced";
        }
        struct LifecycleAction
        {
            Action id{};
            const char* caption{};
        };
        LifecycleAction lifecycle_action(const Snapshot& state)
        {
            if (!state.initialized) {
                return { .id = Initialize, .caption = "Initialize" };
            }
            if (!state.enabled) {
                return { .id = Enable, .caption = "Enable motors" };
            }
            return { .id = Reference, .caption = "Reference robot" };
        }
        const char* provenance(const Snapshot& state, bool live_pose)
        {
            if (!state.pose_valid) {
                return live_pose ? "Frozen pose · reference invalid" : "Nominal CAD · reference required";
            }
            if (!state.fresh) {
                return "Frozen pose · telemetry stale";
            }
            return state.feedback_valid ? "Encoder + INDEX estimates" : "Frozen pose · feedback invalid";
        }
    }
    void View::homePage()
    {
        const auto& s{ m_model.state };
        card(0, layout::home::STATUS_CARD);
        label("System status", layout::home::LABEL_SYSTEM_STATUS);
        lamp(layout::home::STATUS_LAMP, status_color(s));
        label(readiness(s), layout::home::LABEL_CONDITION, true);
        m_labels[m_labelCount - 1].setColor(status_color(s));
        homeAxes();
        homeTCP();
        homeActions();
        homeProvenance();
    }
    void View::homeAxes()
    {
        const auto& s{ m_model.state };
        label("Axes (joints)", layout::home::LABEL_AXES_JOINTS);
        Text<STATUS_TEXT_BYTES> text{};
        for (unsigned i{ 0 }; i < s.axes.size(); ++i) {
            const bool valid{ s.pose_valid && s.axes[i].valid };
            button(Axis1 + i, "", layout::home::AXIS_ROWS[i], true, false, theme::SURFACE, 1);
            format(text, "Axis {}", i + 1);
            label(text.data(), layout::home::AXIS_NAMES[i]);
            auto color{ theme::MUTED };
            if (usable(s)) {
                color = valid ? theme::GREEN : theme::AMBER;
            }
            lamp(layout::home::AXIS_LAMPS[i], color);
            if (valid) {
                format(text, "{:.1f} {}", s.axes[i].feedback, i == LINEAR_AXIS ? "mm" : "°");
            }
            else {
                format(text, "--");
            }
            value(text.data(), layout::home::AXIS_VALUES[i]);
        }
    }
    void View::homeTCP()
    {
        const auto& s{ m_model.state };
        label("Estimated TCP", layout::home::LABEL_ESTIMATED_TCP);
        const bool valid{ s.pose_valid && s.feedback_valid };
        constexpr std::array coordinates{ "X", "Y", "Z" };
        Text<STATUS_TEXT_BYTES> text{};
        for (unsigned i{ 0 }; i < coordinates.size(); ++i) {
            label(coordinates[i], layout::home::TCP_NAMES[i], false, touchgfx::LEFT, Typography::Small);
            if (valid) {
                format(text, "{:.1f} mm", s.feedback_tcp[i]);
            }
            else {
                format(text, "--");
            }
            label(text.data(), layout::home::TCP_VALUES[i], false, touchgfx::RIGHT, Typography::Small);
        }
    }
    void View::homeActions()
    {
        const auto& s{ m_model.state };
        const auto action{ lifecycle_action(s) };
        control(action.id,
                action.caption,
                layout::home::CONTROL_ACTION,
                usable(s) && (action.id != Reference || s.robot_configured),
                true);
        if (s.enabled) {
            control(Disable, "Disable", layout::home::CONTROL_DISABLE, usable(s));
        }
        control(ResetView, "Reset view", layout::home::CONTROL_RESET_VIEW);
    }
    void View::homeProvenance()
    {
        const auto& s{ m_model.state };
        const auto* source{ provenance(s, m_livePose) };
        const auto now{ std::chrono::steady_clock::now().time_since_epoch() };
        Text<STATUS_TEXT_BYTES> text{};
        // Idle images remain valid; show age only while stale or awaiting a new pose/camera.
        if (m_viewport.sourceTimeUs() && (!s.fresh || m_viewport.posePending())) {
            const auto age{ now - std::chrono::microseconds{ m_viewport.sourceTimeUs() } };
            format(text, "{} · image {:.1f} s", source, std::chrono::duration<double>{ age }.count());
        }
        else {
            format(text, "{}", source);
        }
        label(text.data(), layout::home::SCENE_PROVENANCE);
    }
}
