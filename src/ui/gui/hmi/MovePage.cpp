#include "Text.hpp"
#include "View.hpp"
#include "ui/gui/frontend/Theme.hpp"
#include <BitmapDatabase.hpp>
#include <array>
namespace ui::gui::hmi
{
    namespace
    {
        constexpr unsigned MOVE_TEXT_BYTES{ 96 }, POSITION_TEXT_BYTES{ 40 }, MOTOR_NAME_BYTES{ 8 };
        constexpr unsigned CARTESIAN_BUTTONS{ 6 };
        constexpr layout::RepeatedBounds MOTOR_SELECTORS{
            .first = { .x = 124, .y = 160, .width = 88, .height = 56 },
            .dx = 96,
            .dy = 0
        };
        constexpr double SMALL_STEP{ .1 }, LARGE_STEP{ 10 };
        constexpr unsigned JOG_BUTTON_RADIUS{ 52 };
        constexpr std::array AXIS_NAMES{ "Axis 1", "Axis 2", "Axis 3" };
        constexpr std::array BUFFERS{ "Aborting",       "Buffered",   "Blend low",
                                      "Blend previous", "Blend next", "Blend high" };
    }
    void View::movePage()
    {
        control(Axes, "Axes", layout::move::CONTROL_AXES, true, m_model.page == Page::Axes);
        control(
          Cartesian, "Cartesian", layout::move::CONTROL_CARTESIAN, true, m_model.page == Page::Cartesian);
        control(More,
                "More",
                layout::move::CONTROL_MORE,
                true,
                m_model.more || m_model.page == Page::Robot || m_model.page == Page::Preview ||
                  m_model.page == Page::Motors);
        if (m_model.more) {
            card(0, layout::move::MORE_CARD);
            label("Advanced movement", layout::move::LABEL_ADVANCED_MOVEMENT, true);
            control(Robot, "Robot move", layout::move::CONTROL_ROBOT);
            control(Preview, "Path preview", layout::move::CONTROL_PREVIEW);
            control(Motors, "Raw motors", layout::move::CONTROL_MOTORS);
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
        const auto& s{ m_model.state };
        const auto& axis{ s.axes[m_model.axis] };
        card(0, layout::axis::AXIS_CARD);
        card(1, layout::axis::CONTROLS_CARD);
        for (unsigned i{ 0 }; i < 3; ++i) {
            control(Axis1 + i, AXIS_NAMES[i], layout::axis::SELECTORS[i], true, m_model.axis == i);
        }
        Text<MOVE_TEXT_BYTES> text{};
        format(text, "{:.1f} {}", axis.feedback, m_model.axis == 2 ? "mm" : "°");
        label(axis.referenced && axis.valid ? text.data() : "--",
              layout::axis::CURRENT_POSITION,
              false,
              touchgfx::CENTER,
              Typography::Heading);
        constexpr std::array<touchgfx::BitmapId, 3> illustrations{ BITMAP_AXIS_SHOULDER_ID,
                                                                   BITMAP_AXIS_ELBOW_ID,
                                                                   BITMAP_AXIS_Z_ID };
        m_axisImage.setBitmap(touchgfx::Bitmap(illustrations[m_model.axis]));
        m_axisImage.setXY(layout::axis::ILLUSTRATION_X, layout::axis::ILLUSTRATION_Y);
        m_axisImage.setVisible(true);
        format(text,
               "Soft limits   {:.1f} .. {:.1f} {}",
               axis.minimum,
               axis.maximum,
               m_model.axis == 2 ? "mm" : "°");
        label(text.data(), layout::axis::SOFT_LIMITS);
        const char* reference{ "Reference required" };
        if (axis.referenced) {
            reference = axis.encoder ? "Referenced · encoder" : "Referenced · INDEX estimate";
        }
        label(reference, layout::axis::REFERENCE_STATUS);
        control(AxisJogPage, "Jog", layout::axis::CONTROL_AXIS_JOG_PAGE, true, m_model.axis_tab == 0);
        control(AxisPosition, "Move To", layout::axis::CONTROL_AXIS_POSITION, true, m_model.axis_tab == 1);
        control(AxisDynamics, "Settings", layout::axis::CONTROL_AXIS_DYNAMICS, true, m_model.axis_tab == 2);
        switch (m_model.axis_tab) {
            case 0:
                axisJogPage();
                break;
            case 1:
                axisPositionPage();
                break;
            default:
                axisSettingsPage();
                break;
        }
    }
    void View::axisJogPage()
    {
        const auto& s{ m_model.state };
        const bool usable{ s.connected && s.fresh && !s.failed };
        const bool enabled{ usable && s.configured && s.enabled && s.initialized };
        control(StepMode, "Step", layout::axis::CONTROL_STEP_MODE, true, m_model.jog_mode == 0);
        control(Timed, "Timed", layout::axis::CONTROL_TIMED, true, m_model.jog_mode == 1);
        control(Continuous, "Hold", layout::axis::CONTROL_CONTINUOUS, true, m_model.jog_mode == 2);
        button(m_model.jog_mode == 0 ? Minus : JogMinus,
               "−",
               layout::axis::BUTTON_MINUS,
               enabled,
               false,
               0,
               JOG_BUTTON_RADIUS,
               true);
        button(m_model.jog_mode == 0 ? Plus : JogPlus,
               "+",
               layout::axis::BUTTON_PLUS,
               enabled,
               false,
               0,
               JOG_BUTTON_RADIUS,
               true);
        if (m_model.jog_mode == 0) {
            control(StepSmall,
                    m_model.axis == 2 ? "0.1 mm" : "0.1°",
                    layout::axis::CONTROL_STEP_SMALL,
                    true,
                    m_model.step == SMALL_STEP);
            control(StepMedium,
                    m_model.axis == 2 ? "1 mm" : "1°",
                    layout::axis::CONTROL_STEP_MEDIUM,
                    true,
                    m_model.step == 1);
            control(StepLarge,
                    m_model.axis == 2 ? "10 mm" : "10°",
                    layout::axis::CONTROL_STEP_LARGE,
                    true,
                    m_model.step == LARGE_STEP);
        }
        else {
            field(fields::SPEED,
                  m_model.axis == 2 ? "Speed mm/s" : "Speed °/s",
                  m_model.jog_mode == 1 ? layout::axis::TIMED_SPEED : layout::axis::HOLD_SPEED);
            if (m_model.jog_mode == 1) {
                field(fields::DURATION, "Duration s", layout::axis::FIELD_DURATION_S);
            }
        }
        control(Reference, "Reference", layout::axis::CONTROL_REFERENCE, enabled);
        control(AxisStop, "Stop axis", layout::axis::CONTROL_AXIS_STOP, usable);
    }
    void View::axisPositionPage()
    {
        const auto& s{ m_model.state };
        const bool usable{ s.connected && s.fresh && !s.failed };
        const bool enabled{ usable && s.configured && s.enabled && s.initialized };
        control(Relative, "Relative", layout::axis::CONTROL_RELATIVE, true, !m_model.absolute);
        control(Absolute, "Absolute", layout::axis::CONTROL_ABSOLUTE, true, m_model.absolute);
        field(fields::POSITION_1, m_model.absolute ? "Position" : "Distance", layout::axis::DESTINATION);
        field(fields::SPEED, m_model.axis == 2 ? "Speed mm/s" : "Speed °/s", layout::axis::SPEED);
        control(Execute, "Move", layout::axis::CONTROL_EXECUTE, enabled, true);
        control(AxisStop, "Stop", layout::axis::CONTROL_AXIS_STOP_2, usable);
    }
    void View::axisSettingsPage()
    {
        field(fields::ACCELERATION, "Acceleration", layout::axis::FIELD_ACCELERATION);
        field(fields::DECELERATION, "Deceleration", layout::axis::FIELD_DECELERATION);
        field(fields::JERK, "Jerk", layout::axis::FIELD_JERK);
        field(fields::TIMEOUT, "Timeout s", layout::axis::FIELD_TIMEOUT_S);
        label(m_model.axis == 2 ? "mm/s² and mm/s³" : "°/s² and °/s³", layout::axis::DYNAMICS_UNITS);
        label("0 uses configured defaults. Applies to the next move.", layout::axis::DYNAMICS_HELP);
    }
    void View::robotPage()
    {
        const auto& s{ m_model.state };
        const bool enabled{ s.connected && s.fresh && !s.failed && s.robot_configured && s.initialized &&
                            s.enabled };
        if (m_model.page == Page::Cartesian) {
            cartesianPage();
            return;
        }
        card(0, layout::robot::CONTROLS_CARD);
        control(PreviewJoints,
                "Joint destination",
                layout::robot::CONTROL_PREVIEW_JOINTS,
                true,
                m_model.coordinates == 0);
        control(PreviewPoint,
                "Cartesian destination",
                layout::robot::CONTROL_PREVIEW_POINT,
                true,
                m_model.coordinates == 1);
        if (m_model.page == Page::Preview) {
            control(PreviewLine,
                    "Straight line",
                    layout::robot::CONTROL_PREVIEW_LINE,
                    true,
                    m_model.coordinates == 2);
        }
        else {
            control(Advanced,
                    m_model.advanced ? "Destination" : "Advanced",
                    layout::robot::CONTROL_ADVANCED,
                    true,
                    m_model.advanced);
        }
        if (m_model.advanced) {
            field(fields::ACCELERATION, "Acceleration factor", layout::robot::FIELD_ACCELERATION_FACTOR);
            field(fields::DECELERATION, "Deceleration factor", layout::robot::FIELD_DECELERATION_FACTOR);
            field(fields::JERK, "Jerk factor", layout::robot::FIELD_JERK_FACTOR);
            field(fields::TIMEOUT, "Timeout s", layout::robot::FIELD_TIMEOUT_S);
            label("Factors scale configured limits (0 = 1). They apply to the next move.",
                  layout::robot::DYNAMICS_HELP);
            return;
        }
        field(fields::POSITION_1,
              m_model.coordinates == 0 ? "Axis 1 °" : "X mm",
              layout::robot::FIRST_COORDINATE);
        field(fields::POSITION_2,
              m_model.coordinates == 0 ? "Axis 2 °" : "Y mm",
              layout::robot::SECOND_COORDINATE);
        field(fields::POSITION_3,
              m_model.coordinates == 0 ? "Axis 3 mm" : "Z mm",
              layout::robot::THIRD_COORDINATE);
        field(fields::SPEED_FACTOR, "Speed factor", layout::robot::FIELD_SPEED_FACTOR);
        if (m_model.page == Page::Robot) {
            control(Relative, "Relative", layout::robot::CONTROL_RELATIVE, true, !m_model.absolute);
            control(Absolute, "Absolute", layout::robot::CONTROL_ABSOLUTE, true, m_model.absolute);
            control(Reference, "Reference robot", layout::robot::CONTROL_REFERENCE, enabled);
            control(Execute, "Move to destination", layout::robot::CONTROL_EXECUTE, enabled, true);
            label("Joint interpolation · TCP path may curve", layout::robot::PATH_HELP);
        }
        else {
            control(Advanced, "Dynamics", layout::robot::CONTROL_ADVANCED_2);
            control(RunPreview,
                    "Calculate",
                    layout::robot::CONTROL_RUN_PREVIEW,
                    s.connected && s.robot_configured,
                    true);
            label("Preview only · no motor movement", layout::robot::LABEL_PREVIEW_ONLY_NO_MOTOR_MOVEMENT);
            control(TogglePreviews, "Preview results", layout::robot::CONTROL_TOGGLE_PREVIEWS);
        }
    }
    void View::cartesianPage()
    {
        const auto& s{ m_model.state };
        const bool fresh{ s.connected && s.fresh && !s.failed };
        const bool enabled{ fresh && s.robot_configured && s.initialized && s.enabled && s.pose_valid };
        const bool position_valid{ fresh && s.robot_configured && s.pose_valid && s.feedback_valid };
        card(0, layout::cartesian::POSITION_CARD);
        card(1, layout::cartesian::CONTROLS_CARD);
        label("Estimated TCP", layout::cartesian::LABEL_ESTIMATED_TCP, true, touchgfx::CENTER);
        constexpr std::array coordinates{ "X", "Y", "Z" };
        for (unsigned i{ 0 }; i < 3; ++i) {
            Text<POSITION_TEXT_BYTES> text{};
            label(coordinates[i], layout::cartesian::POSITION_NAMES[i], true);
            if (position_valid) {
                format(text, "{:.2f} mm", s.feedback_tcp[i]);
            }
            else {
                format(text, "--");
            }
            label(text.data(), layout::cartesian::POSITION_VALUES[i], true, touchgfx::RIGHT);
        }
        const char* source{ "Encoder + INDEX estimate" };
        if (!fresh) {
            source = "Feedback unavailable / stale";
        }
        else if (!s.robot_configured) {
            source = "Robot configuration required";
        }
        else if (!s.pose_valid) {
            source = "Reference required";
        }
        else if (!s.feedback_valid) {
            source = "Feedback unavailable";
        }
        label(source, layout::cartesian::LABEL_SOURCE, false, touchgfx::LEFT, Typography::Small);
        label(m_model.cartesian_tab == 0 && m_model.cartesian_jog_mode != 0
                ? "Straight jog; stops at certified boundary."
                : "Joint interpolation; TCP path may curve.",
              layout::cartesian::PATH_HELP,
              false,
              touchgfx::LEFT,
              Typography::Small);
        control(CartesianJogPage,
                "Jog",
                layout::cartesian::CONTROL_CARTESIAN_JOG_PAGE,
                true,
                m_model.cartesian_tab == 0);
        control(CartesianPosition,
                "Move To",
                layout::cartesian::CONTROL_CARTESIAN_POSITION,
                true,
                m_model.cartesian_tab == 1);
        control(CartesianDynamics,
                "Settings",
                layout::cartesian::CONTROL_CARTESIAN_DYNAMICS,
                true,
                m_model.cartesian_tab == 2);
        if (m_model.cartesian_tab == 0) {
            cartesianJogPage();
        }
        else if (m_model.cartesian_tab == 1) {
            control(Relative, "Relative", layout::cartesian::CONTROL_RELATIVE, true, !m_model.absolute);
            control(Absolute, "Absolute", layout::cartesian::CONTROL_ABSOLUTE, true, m_model.absolute);
            field(fields::POSITION_1, "X mm", layout::cartesian::FIELD_X_MM);
            field(fields::POSITION_2, "Y mm", layout::cartesian::FIELD_Y_MM);
            field(fields::POSITION_3, "Z mm", layout::cartesian::FIELD_Z_MM);
            field(fields::SPEED_FACTOR, "Speed factor", layout::cartesian::FIELD_SPEED_FACTOR_2);
            control(Execute, "Move", layout::cartesian::CONTROL_EXECUTE, enabled, true);
            control(GlobalStop, "Stop", layout::cartesian::CONTROL_GLOBAL_STOP, fresh);
        }
        else {
            field(fields::ACCELERATION, "Acceleration factor", layout::cartesian::FIELD_ACCELERATION_FACTOR);
            field(fields::DECELERATION, "Deceleration factor", layout::cartesian::FIELD_DECELERATION_FACTOR);
            field(fields::JERK, "Jerk factor", layout::cartesian::FIELD_JERK_FACTOR);
            field(fields::TIMEOUT, "Timeout s", layout::cartesian::FIELD_TIMEOUT_S);
            field(fields::SPEED_FACTOR, "Speed factor", layout::cartesian::FIELD_SPEED_FACTOR_3);
            label("Factors scale joint limits.\n0 uses defaults.", layout::cartesian::DYNAMICS_HELP);
            label("Applies to the next movement.", layout::cartesian::LABEL_APPLIES_TO_THE_NEXT_MOVEMENT);
        }
    }
    void View::cartesianJogPage()
    {
        const auto& s{ m_model.state };
        const bool enabled{ s.connected && s.fresh && !s.failed && s.robot_configured && s.initialized &&
                            s.enabled && s.pose_valid };
        constexpr std::array colors{ theme::X_AXIS, theme::Y_AXIS, theme::Z_AXIS };
        control(
          StepMode, "Step", layout::cartesian::CONTROL_STEP_MODE, true, m_model.cartesian_jog_mode == 0);
        control(Timed, "Timed", layout::cartesian::CONTROL_TIMED, true, m_model.cartesian_jog_mode == 1);
        control(
          Continuous, "Hold", layout::cartesian::CONTROL_CONTINUOUS, true, m_model.cartesian_jog_mode == 2);
        constexpr std::array names{ "X −", "X +", "Y −", "Y +", "Z −", "Z +" };
        for (unsigned i{ 0 }; i < CARTESIAN_BUTTONS; ++i) {
            button(XMinus + i, names[i], layout::cartesian::JOG_BUTTONS[i], enabled, false, colors[i / 2]);
        }
        if (m_model.cartesian_jog_mode == 0) {
            label("Step size", layout::cartesian::LABEL_STEP_SIZE);
            control(StepSmall,
                    "0.1 mm",
                    layout::cartesian::CONTROL_STEP_SMALL,
                    true,
                    m_model.cartesian_step == SMALL_STEP);
            control(
              StepMedium, "1 mm", layout::cartesian::CONTROL_STEP_MEDIUM, true, m_model.cartesian_step == 1);
            control(StepLarge,
                    "10 mm",
                    layout::cartesian::CONTROL_STEP_LARGE,
                    true,
                    m_model.cartesian_step == LARGE_STEP);
        }
        else {
            field(fields::SPEED_FACTOR, "Speed factor", layout::cartesian::FIELD_SPEED_FACTOR);
            if (m_model.cartesian_jog_mode == 1) {
                field(fields::DURATION, "Duration s", layout::cartesian::FIELD_DURATION_S);
            }
            else {
                label("Press and hold.\nRelease to stop.",
                      layout::cartesian::LABEL_PRESS_AND_HOLD_N_RELEASE_TO_STOP);
            }
        }
    }
    void View::motorPage()
    {
        const auto& s{ m_model.state };
        const bool usable{ s.connected && s.fresh && !s.failed };
        card(0, layout::motor::CONTROLS_CARD);
        for (unsigned i{ 0 }; i < 3; ++i) {
            Text<MOTOR_NAME_BYTES> text{};
            format(text, "M{}", i + 1);
            control(Axis1 + i, text.data(), MOTOR_SELECTORS[i], true, m_model.axis == i);
        }
        control(MotorMoveTab, "Move", layout::motor::CONTROL_MOTOR_MOVE_TAB, true, m_model.motor_tab == 0);
        control(MotorHomeTab, "Home", layout::motor::CONTROL_MOTOR_HOME_TAB, true, m_model.motor_tab == 1);
        control(MotorDefaultsTab,
                "Defaults",
                layout::motor::CONTROL_MOTOR_DEFAULTS_TAB,
                true,
                m_model.motor_tab == 2);
        Text<MOVE_TEXT_BYTES> text{};
        const auto& m{ s.motor(m_model.axis) };
        format(text,
               "Shaft {:.2f}°    Speed {:.2f} rpm    {}",
               m.shaft,
               m.rpm,
               m.encoder ? "Encoder" : "INDEX estimate");
        label(text.data(), layout::motor::MOTOR_STATE);
        if (m_model.motor_tab == 0) {
            if (m_model.advanced) {
                field(fields::ACCELERATION, "Accel °/s²", layout::motor::FIELD_ACCEL_S);
                field(fields::DECELERATION, "Decel °/s²", layout::motor::FIELD_DECEL_S);
                field(fields::JERK, "Jerk °/s³", layout::motor::FIELD_JERK_S);
                field(fields::TIMEOUT, "Timeout s", layout::motor::FIELD_TIMEOUT_S);
                label("Applies to the next motor move.", layout::motor::LABEL_APPLIES_TO_THE_NEXT_MOTOR_MOVE);
                control(Advanced, "Back to movement", layout::motor::CONTROL_ADVANCED);
                control(Abort, "ABORT", layout::motor::CONTROL_ABORT, usable);
            }
            else {
                field(fields::POSITION_1, "Shaft degrees", layout::motor::FIELD_SHAFT_DEGREES);
                field(fields::SPEED, "Speed rpm", layout::motor::FIELD_SPEED_RPM);
                control(NextBuffer, BUFFERS[m_model.buffer], layout::motor::CONTROL_NEXT_BUFFER);
                control(Relative, "Relative", layout::motor::CONTROL_RELATIVE, true, !m_model.absolute);
                control(Absolute, "Absolute", layout::motor::CONTROL_ABSOLUTE, true, m_model.absolute);
                control(ActiveSpeed, "Set active speed", layout::motor::CONTROL_ACTIVE_SPEED, usable);
                control(Advanced, "Dynamics", layout::motor::CONTROL_ADVANCED_2);
                control(Execute, "Move motor", layout::motor::CONTROL_EXECUTE, usable && s.enabled, true);
                control(Abort, "ABORT", layout::motor::CONTROL_ABORT, usable);
            }
        }
        else if (m_model.motor_tab == 1) {
            field(fields::SPEED, "Seek rpm", layout::motor::FIELD_SEEK_RPM);
            field(fields::LATCH_SPEED, "Latch rpm", layout::motor::FIELD_LATCH_RPM);
            field(fields::TIMEOUT, "Timeout s", layout::motor::FIELD_TIMEOUT_S_2);
            label("Reference uses seek, release and slow re-latch.", layout::motor::HOMING_HELP);
            control(
              Reference, "Reference motor", layout::motor::CONTROL_REFERENCE, usable && s.enabled, true);
            control(Abort, "ABORT", layout::motor::CONTROL_ABORT, usable);
        }
        else {
            field(fields::ACCELERATION, "Accel °/s²", layout::motor::FIELD_ACCEL_S_2);
            field(fields::DECELERATION, "Decel °/s²", layout::motor::FIELD_DECEL_S_2);
            field(fields::JERK, "Jerk °/s³", layout::motor::FIELD_JERK_S_2);
            field(fields::TIMEOUT, "Timeout s", layout::motor::FIELD_TIMEOUT_S_3);
            label("Save defaults only while this motor is idle.", layout::motor::DEFAULTS_HELP);
            control(ApplyDefaults,
                    "Save defaults",
                    layout::motor::CONTROL_APPLY_DEFAULTS,
                    usable && !m.owner,
                    true);
            control(Abort, "ABORT", layout::motor::CONTROL_ABORT, usable);
        }
    }
}
