#include "View.hpp"
#include "ui/gui/frontend/Theme.hpp"
#include <BitmapDatabase.hpp>
#include <cstdio>
namespace ui::gui::hmi
{
    namespace
    {
        constexpr const char* axisNames[]{ "Axis 1", "Axis 2", "Axis 3" };
        constexpr const char* buffers[]{ "Aborting",       "Buffered",   "Blend low",
                                         "Blend previous", "Blend next", "Blend high" };
    }
    void View::movePage()
    {
        control(Axes, "Axes", 112, 84, 324, true, m_model.page == Page::Axes);
        control(Cartesian, "Cartesian", 448, 84, 324, true, m_model.page == Page::Cartesian);
        control(More,
                "More",
                784,
                84,
                224,
                true,
                m_model.more || m_model.page == Page::Robot || m_model.page == Page::Preview ||
                  m_model.page == Page::Motors);
        if (m_model.more) {
            card(0, 560, 148, 448, 336);
            label("Advanced movement", 584, 168, 400, 36, true);
            control(Robot, "Robot move", 584, 224, 400);
            control(Preview, "Path preview", 584, 300, 400);
            control(Motors, "Raw motors", 584, 376, 400);
            return;
        }
        switch (m_model.page) {
            case Page::Axes:
                axisPage();
                break;
            case Page::Motors:
                motorPage();
                break;
            default:
                robotPage();
                break;
        }
    }
    void View::axisPage()
    {
        const auto& s = m_model.state;
        const auto& axis = s.axes[m_model.axis];
        const bool usable = s.connected && s.fresh && !s.failed;
        const bool enabled = usable && s.configured && s.enabled && s.initialized;
        card(0, 112, 148, 324, 376);
        card(1, 448, 148, 560, 376);
        for (unsigned i = 0; i < 3; ++i)
            control(Axis1 + i, axisNames[i], 120 + int(i) * 104, 156, 100, true, m_model.axis == i);
        char text[96];
        std::snprintf(text, sizeof(text), "%.1f %s", axis.feedback, m_model.axis == 2 ? "mm" : "°");
        label(axis.referenced && axis.valid ? text : "--",
              132,
              218,
              284,
              44,
              false,
              touchgfx::CENTER,
              Typography::Heading);
        constexpr touchgfx::BitmapId illustrations[]{ BITMAP_AXIS_SHOULDER_ID,
                                                      BITMAP_AXIS_ELBOW_ID,
                                                      BITMAP_AXIS_Z_ID };
        m_axisImage.setBitmap(touchgfx::Bitmap(illustrations[m_model.axis]));
        m_axisImage.setXY(122, 268);
        m_axisImage.setVisible(true);
        std::snprintf(text,
                      sizeof(text),
                      "Soft limits   %.1f .. %.1f %s",
                      axis.minimum,
                      axis.maximum,
                      m_model.axis == 2 ? "mm" : "°");
        label(text, 128, 458, 292, 28);
        label(axis.referenced ? (axis.encoder ? "Referenced · encoder" : "Referenced · INDEX estimate")
                              : "Reference required",
              128,
              488,
              292,
              28);
        control(AxisJogPage, "Jog", 456, 156, 176, true, m_model.axisTab == 0);
        control(AxisPosition, "Move To", 640, 156, 176, true, m_model.axisTab == 1);
        control(AxisDynamics, "Settings", 824, 156, 176, true, m_model.axisTab == 2);
        if (m_model.axisTab == 0) {
            control(StepMode, "Step", 456, 220, 176, true, m_model.jogMode == 0);
            control(Timed, "Timed", 640, 220, 176, true, m_model.jogMode == 1);
            control(Continuous, "Hold", 824, 220, 176, true, m_model.jogMode == 2);
            button(
              m_model.jogMode == 0 ? Minus : JogMinus, "−", 520, 284, 104, 104, enabled, false, 0, 52, true);
            button(
              m_model.jogMode == 0 ? Plus : JogPlus, "+", 824, 284, 104, 104, enabled, false, 0, 52, true);
            if (m_model.jogMode == 0) {
                control(
                  StepSmall, m_model.axis == 2 ? "0.1 mm" : "0.1°", 456, 396, 176, true, m_model.step == .1);
                control(
                  StepMedium, m_model.axis == 2 ? "1 mm" : "1°", 640, 396, 176, true, m_model.step == 1);
                control(
                  StepLarge, m_model.axis == 2 ? "10 mm" : "10°", 824, 396, 176, true, m_model.step == 10);
            }
            else {
                field(3,
                      m_model.axis == 2 ? "Speed mm/s" : "Speed °/s",
                      456,
                      396,
                      m_model.jogMode == 1 ? 268 : 544);
                if (m_model.jogMode == 1)
                    field(8, "Duration s", 736, 396, 264);
            }
            control(Reference, "Reference", 456, 460, 264, enabled);
            control(AxisStop, "Stop axis", 736, 460, 264, usable);
        }
        else if (m_model.axisTab == 1) {
            control(Relative, "Relative", 456, 220, 264, true, !m_model.absolute);
            control(Absolute, "Absolute", 736, 220, 264, true, m_model.absolute);
            field(0, m_model.absolute ? "Position" : "Distance", 456, 292, 544);
            field(3, m_model.axis == 2 ? "Speed mm/s" : "Speed °/s", 456, 364, 544);
            control(Execute, "Move", 456, 460, 360, enabled, true);
            control(AxisStop, "Stop", 832, 460, 168, usable);
        }
        else {
            field(4, "Acceleration", 456, 230, 264);
            field(5, "Deceleration", 736, 230, 264);
            field(6, "Jerk", 456, 310, 264);
            field(7, "Timeout s", 736, 310, 264);
            label(m_model.axis == 2 ? "mm/s² and mm/s³" : "°/s² and °/s³", 468, 390, 520);
            label("0 uses configured defaults. Applies to the next move.", 468, 432, 520, 72);
        }
    }
    void View::robotPage()
    {
        const auto& s = m_model.state;
        const bool enabled =
          s.connected && s.fresh && !s.failed && s.robotConfigured && s.initialized && s.enabled;
        if (m_model.page == Page::Cartesian) {
            cartesianPage();
            return;
        }
        card(0, 112, 148, 896, 376);
        control(PreviewJoints, "Joint destination", 124, 160, 276, true, m_model.coordinates == 0);
        control(PreviewPoint, "Cartesian destination", 412, 160, 276, true, m_model.coordinates == 1);
        if (m_model.page == Page::Preview)
            control(PreviewLine, "Straight line", 700, 160, 296, true, m_model.coordinates == 2);
        else
            control(
              Advanced, m_model.advanced ? "Destination" : "Advanced", 700, 160, 296, true, m_model.advanced);
        if (m_model.advanced) {
            field(4, "Acceleration factor", 128, 244, 268);
            field(5, "Deceleration factor", 420, 244, 268);
            field(6, "Jerk factor", 712, 244, 280);
            field(7, "Timeout s", 128, 340, 268);
            label("Factors scale configured limits (0 = 1). They apply to the next move.", 420, 340, 568, 80);
            return;
        }
        field(0, m_model.coordinates == 0 ? "Axis 1 °" : "X mm", 128, 236, 268);
        field(1, m_model.coordinates == 0 ? "Axis 2 °" : "Y mm", 420, 236, 268);
        field(2, m_model.coordinates == 0 ? "Axis 3 mm" : "Z mm", 712, 236, 280);
        field(10, "Speed factor", 128, 316, 268);
        if (m_model.page == Page::Robot) {
            control(Relative, "Relative", 420, 316, 268, true, !m_model.absolute);
            control(Absolute, "Absolute", 712, 316, 280, true, m_model.absolute);
            control(Reference, "Reference robot", 128, 460, 268, enabled);
            control(Execute, "Move to destination", 712, 460, 280, enabled, true);
            label("Joint interpolation · TCP path may curve", 128, 398, 864);
        }
        else {
            control(Advanced, "Dynamics", 420, 316, 268);
            control(RunPreview, "Calculate", 712, 316, 280, s.connected && s.robotConfigured, true);
            label("Preview only · no motor movement", 128, 400, 864);
            control(TogglePreviews, "Preview results", 128, 460, 268);
        }
    }
    void View::cartesianPage()
    {
        const auto& s = m_model.state;
        const bool fresh = s.connected && s.fresh && !s.failed;
        const bool enabled = fresh && s.robotConfigured && s.initialized && s.enabled && s.poseValid;
        const bool positionValid = fresh && s.robotConfigured && s.poseValid && s.feedbackValid;
        card(0, 112, 148, 324, 376);
        card(1, 448, 148, 560, 376);
        label("Estimated TCP", 132, 168, 284, 36, true, touchgfx::CENTER);
        constexpr const char* coordinates[]{ "X", "Y", "Z" };
        constexpr std::uint32_t colors[]{ theme::X_AXIS, theme::Y_AXIS, theme::Z_AXIS };
        for (unsigned i = 0; i < 3; ++i) {
            char text[40];
            label(coordinates[i], 132, 226 + int(i) * 64, 32, 36, true);
            std::snprintf(text, sizeof(text), positionValid ? "%.2f mm" : "--", s.feedbackTcp[i]);
            label(text, 172, 222 + int(i) * 64, 244, 44, true, touchgfx::RIGHT);
        }
        const char* source = !fresh               ? "Feedback unavailable / stale"
                             : !s.robotConfigured ? "Robot configuration required"
                             : !s.poseValid       ? "Reference required"
                             : !s.feedbackValid   ? "Feedback unavailable"
                                                  : "Encoder + INDEX estimate";
        label(source, 128, 420, 292, 40, false, touchgfx::LEFT, Typography::Small);
        label("Joint interpolation; TCP path may curve.",
              128,
              468,
              292,
              48,
              false,
              touchgfx::LEFT,
              Typography::Small);
        control(CartesianJogPage, "Jog", 456, 156, 176, true, m_model.cartesianTab == 0);
        control(CartesianPosition, "Move To", 640, 156, 176, true, m_model.cartesianTab == 1);
        control(CartesianDynamics, "Settings", 824, 156, 176, true, m_model.cartesianTab == 2);
        if (m_model.cartesianTab == 0) {
            control(StepMode, "Step", 456, 220, 176, true, m_model.cartesianJogMode == 0);
            control(Timed, "Timed", 640, 220, 176, true, m_model.cartesianJogMode == 1);
            control(Continuous, "Hold", 824, 220, 176, true, m_model.cartesianJogMode == 2);
            constexpr const char* names[]{ "X −", "X +", "Y −", "Y +", "Z −", "Z +" };
            for (unsigned i = 0; i < 6; ++i)
                button(XMinus + i,
                       names[i],
                       456 + int(i % 2) * 128,
                       292 + int(i / 2) * 72,
                       120,
                       64,
                       enabled,
                       false,
                       colors[i / 2]);
            if (m_model.cartesianJogMode == 0) {
                label("Step size", 728, 284, 264);
                control(StepSmall, "0.1 mm", 720, 320, 280, true, m_model.cartesianStep == .1);
                control(StepMedium, "1 mm", 720, 384, 280, true, m_model.cartesianStep == 1);
                control(StepLarge, "10 mm", 720, 448, 280, true, m_model.cartesianStep == 10);
            }
            else {
                field(10, "Speed factor", 720, 300, 280);
                if (m_model.cartesianJogMode == 1)
                    field(8, "Duration s", 720, 388, 280);
                else
                    label("Press and hold.\nRelease to stop.", 728, 388, 264, 72);
            }
        }
        else if (m_model.cartesianTab == 1) {
            control(Relative, "Relative", 456, 220, 264, true, !m_model.absolute);
            control(Absolute, "Absolute", 736, 220, 264, true, m_model.absolute);
            field(0, "X mm", 456, 292, 168);
            field(1, "Y mm", 640, 292, 168);
            field(2, "Z mm", 824, 292, 176);
            field(10, "Speed factor", 456, 364, 544);
            control(Execute, "Move", 456, 460, 360, enabled, true);
            control(GlobalStop, "Stop", 832, 460, 168, fresh);
        }
        else {
            field(4, "Acceleration factor", 456, 230, 264);
            field(5, "Deceleration factor", 736, 230, 264);
            field(6, "Jerk factor", 456, 302, 264);
            field(7, "Timeout s", 736, 302, 264);
            field(10, "Speed factor", 456, 374, 264);
            label("Factors scale joint limits.\n0 uses defaults.", 736, 370, 264, 80);
            label("Applies to the next movement.", 468, 464, 520, 40);
        }
    }
    void View::motorPage()
    {
        const auto& s = m_model.state;
        const bool usable = s.connected && s.fresh && !s.failed;
        card(0, 112, 148, 896, 376);
        for (unsigned i = 0; i < 3; ++i) {
            char text[8];
            std::snprintf(text, sizeof(text), "M%u", i + 1);
            control(Axis1 + i, text, 124 + int(i) * 96, 160, 88, true, m_model.axis == i);
        }
        control(MotorMoveTab, "Move", 424, 160, 180, true, m_model.motorTab == 0);
        control(MotorHomeTab, "Home", 616, 160, 180, true, m_model.motorTab == 1);
        control(MotorDefaultsTab, "Defaults", 808, 160, 188, true, m_model.motorTab == 2);
        char text[96];
        const auto& m = s.motor(m_model.axis);
        std::snprintf(text,
                      sizeof(text),
                      "Shaft %.2f°    Speed %.2f rpm    %s",
                      m.shaft,
                      m.rpm,
                      m.encoder ? "Encoder" : "INDEX estimate");
        label(text, 132, 228, 864);
        if (m_model.motorTab == 0) {
            if (m_model.advanced) {
                field(4, "Accel °/s²", 128, 274, 268);
                field(5, "Decel °/s²", 420, 274, 268);
                field(6, "Jerk °/s³", 712, 274, 280);
                field(7, "Timeout s", 128, 354, 268);
                label("Applies to the next motor move.", 420, 374, 568);
                control(Advanced, "Back to movement", 128, 454, 560);
                control(Abort, "ABORT", 712, 454, 280, usable);
            }
            else {
                field(0, "Shaft degrees", 128, 274, 268);
                field(3, "Speed rpm", 420, 274, 268);
                control(NextBuffer, buffers[m_model.buffer], 712, 274, 280);
                control(Relative, "Relative", 128, 346, 268, true, !m_model.absolute);
                control(Absolute, "Absolute", 420, 346, 268, true, m_model.absolute);
                control(ActiveSpeed, "Set active speed", 712, 346, 280, usable);
                control(Advanced, "Dynamics", 128, 454, 268);
                control(Execute, "Move motor", 420, 454, 268, usable && s.enabled, true);
                control(Abort, "ABORT", 712, 454, 280, usable);
            }
        }
        else if (m_model.motorTab == 1) {
            field(3, "Seek rpm", 128, 284, 268);
            field(9, "Latch rpm", 420, 284, 268);
            field(7, "Timeout s", 712, 284, 280);
            label("Reference uses seek, release and slow re-latch.", 132, 378, 856);
            control(Reference, "Reference motor", 128, 454, 560, usable && s.enabled, true);
            control(Abort, "ABORT", 712, 454, 280, usable);
        }
        else {
            field(4, "Accel °/s²", 128, 284, 268);
            field(5, "Decel °/s²", 420, 284, 268);
            field(6, "Jerk °/s³", 712, 284, 280);
            field(7, "Timeout s", 128, 364, 268);
            label("Save defaults only while this motor is idle.", 420, 378, 568);
            control(ApplyDefaults, "Save defaults", 128, 454, 560, usable && !m.owner, true);
            control(Abort, "ABORT", 712, 454, 280, usable);
        }
    }
}
