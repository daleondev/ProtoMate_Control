#pragma once
#include <cstdint>

namespace ui::gui::hmi::layout
{
    // Logical pixels at the fixed 1024 x 600 canvas. Keep presentation geometry
    // separate from command/readiness logic; these constants allocate no widgets.
    struct Bounds
    {
        int x{}, y{}, width{}, height{};
    };
    struct RepeatedBounds
    {
        Bounds first{};
        int dx{}, dy{};
        constexpr Bounds operator[](unsigned index) const
        {
            auto result{ first };
            result.x += static_cast<int>(index) * dx;
            result.y += static_cast<int>(index) * dy;
            return result;
        }
    };
    struct GridBounds
    {
        Bounds first{};
        unsigned columns{ 1 };
        int dx{}, dy{};
        constexpr Bounds operator[](unsigned index) const
        {
            auto result{ first };
            result.x += static_cast<int>(index % columns) * dx;
            result.y += static_cast<int>(index / columns) * dy;
            return result;
        }
    };
    namespace shell
    {
        inline constexpr Bounds CANVAS{ .width = 1024, .height = 600 };
        inline constexpr Bounds NAVIGATION{ .x = 8, .y = 8, .width = 88, .height = 584 };
        inline constexpr Bounds HEADER{ .x = 104, .y = 8, .width = 912, .height = 64 };
        inline constexpr Bounds CONTENT{ .x = 104, .y = 80, .width = 912, .height = 444 };
        inline constexpr Bounds FOOTER{ .x = 104, .y = 532, .width = 912, .height = 60 };
        inline constexpr RepeatedBounds NAV_BUTTONS{ .first = { .x = 12, .y = 12, .width = 80, .height = 92 },
                                                     .dy = 96 };
        inline constexpr Bounds JOBS{ .x = 112, .y = 534, .width = 104, .height = 56 };
        inline constexpr Bounds STOP{ .x = 880, .y = 534, .width = 128, .height = 56 };
        inline constexpr Bounds TITLE{ .x = 124, .y = 25, .width = 490, .height = 40 };
        inline constexpr Bounds HEALTH{ .x = 622, .y = 28, .width = 374, .height = 28 };
        inline constexpr Bounds NOTICE{ .x = 232, .y = 545, .width = 632, .height = 38 };
        inline constexpr RepeatedBounds COLOR_BARS{ .first = { .width = 128, .height = 528 }, .dx = 128 };
        inline constexpr unsigned NAV_COUNT{ 6 }, JOBS_INDEX{ 6 }, STOP_INDEX{ 7 }, PANEL_COUNT{ 4 };
        inline constexpr int CONTROL_HEIGHT{ 56 }, VALUE_HEIGHT{ 28 }, LARGE_VALUE_HEIGHT{ 44 },
          LAMP_SIZE{ 12 };
        inline constexpr unsigned LAMP_RADIUS{ 6 };
    }
    namespace dialog
    {
        inline constexpr Bounds CONFIRM_TITLE{ .x = 160, .y = 160, .width = 800, .height = 44 };
        inline constexpr Bounds CONFIRM_HELP{ .x = 160, .y = 232, .width = 800, .height = 32 };
        inline constexpr Bounds REJECT{ .x = 240, .y = 352, .width = 280, .height = 56 };
        inline constexpr Bounds CONFIRM{ .x = 552, .y = 352, .width = 240, .height = 56 };
        inline constexpr Bounds FIELD_NAME{ .x = 160, .y = 98, .width = 800, .height = 32 };
        inline constexpr Bounds VALUE{ .x = 200, .y = 145, .width = 660, .height = 44 };
        inline constexpr GridBounds DIGITS{ .first = { .x = 200, .y = 210, .width = 120, .height = 56 },
                                            .columns = 3,
                                            .dx = 136,
                                            .dy = 64 };
        inline constexpr Bounds SIGN{ .x = 200, .y = 402, .width = 120, .height = 56 };
        inline constexpr Bounds ZERO{ .x = 336, .y = 402, .width = 120, .height = 56 };
        inline constexpr Bounds DOT{ .x = 472, .y = 402, .width = 120, .height = 56 };
        inline constexpr Bounds DELETE{ .x = 660, .y = 210, .width = 200, .height = 56 };
        inline constexpr Bounds CANCEL{ .x = 660, .y = 338, .width = 200, .height = 56 };
        inline constexpr Bounds APPLY{ .x = 660, .y = 402, .width = 200, .height = 56 };
        inline constexpr Bounds HELP{ .x = 160, .y = 480, .width = 820, .height = 32 };
    }
    template<class Widget>
    void position(Widget& widget, Bounds bounds)
    {
        widget.setPosition(static_cast<std::int16_t>(bounds.x),
                           static_cast<std::int16_t>(bounds.y),
                           static_cast<std::int16_t>(bounds.width),
                           static_cast<std::int16_t>(bounds.height));
    }
    namespace home
    {
        inline constexpr Bounds STATUS_LAMP{ .x = 768, .y = 104 };
        inline constexpr RepeatedBounds AXIS_ROWS{
            .first = { .x = 760, .y = 198, .width = 248, .height = 56 },
            .dy = 56
        };
        inline constexpr RepeatedBounds AXIS_NAMES{
            .first = { .x = 792, .y = 212, .width = 80, .height = 28 },
            .dy = 56
        };
        inline constexpr RepeatedBounds AXIS_LAMPS{ .first = { .x = 772, .y = 221 }, .dy = 56 };
        inline constexpr RepeatedBounds AXIS_VALUES{ .first = { .x = 872, .y = 212, .width = 124 },
                                                     .dy = 56 };
        inline constexpr RepeatedBounds TCP_NAMES{ .first = { .x = 768, .y = 402, .width = 32, .height = 22 },
                                                   .dy = 20 };
        inline constexpr RepeatedBounds TCP_VALUES{
            .first = { .x = 810, .y = 402, .width = 186, .height = 22 },
            .dy = 20
        };
        inline constexpr Bounds STATUS_CARD{ .x = 752, .y = 80, .width = 264, .height = 444 };
        inline constexpr Bounds LABEL_SYSTEM_STATUS{ .x = 792, .y = 96, .width = 208, .height = 32 };
        inline constexpr Bounds LABEL_CONDITION{ .x = 768, .y = 126, .width = 232, .height = 36 };
        inline constexpr Bounds LABEL_AXES_JOINTS{ .x = 768, .y = 170, .width = 232, .height = 32 };
        inline constexpr Bounds LABEL_ESTIMATED_TCP{ .x = 768, .y = 374, .width = 232, .height = 32 };
        inline constexpr Bounds CONTROL_ACTION{ .x = 764, .y = 464, .width = 240, .height = 56 };
        inline constexpr Bounds CONTROL_DISABLE{ .x = 568, .y = 82, .width = 168, .height = 56 };
        inline constexpr Bounds CONTROL_RESET_VIEW{ .x = 568, .y = 460, .width = 168, .height = 56 };
        inline constexpr Bounds SCENE_PROVENANCE{ .x = 120, .y = 494, .width = 620, .height = 28 };
    }
    namespace move
    {
        inline constexpr Bounds CONTROL_AXES{ .x = 112, .y = 84, .width = 324, .height = 56 };
        inline constexpr Bounds CONTROL_CARTESIAN{ .x = 448, .y = 84, .width = 324, .height = 56 };
        inline constexpr Bounds CONTROL_MORE{ .x = 784, .y = 84, .width = 224, .height = 56 };
        inline constexpr Bounds MORE_CARD{ .x = 560, .y = 148, .width = 448, .height = 336 };
        inline constexpr Bounds LABEL_ADVANCED_MOVEMENT{ .x = 584, .y = 168, .width = 400, .height = 36 };
        inline constexpr Bounds CONTROL_ROBOT{ .x = 584, .y = 224, .width = 400, .height = 56 };
        inline constexpr Bounds CONTROL_PREVIEW{ .x = 584, .y = 300, .width = 400, .height = 56 };
        inline constexpr Bounds CONTROL_MOTORS{ .x = 584, .y = 376, .width = 400, .height = 56 };
    }
    namespace axis
    {
        inline constexpr RepeatedBounds SELECTORS{
            .first = { .x = 120, .y = 156, .width = 100, .height = 56 },
            .dx = 104
        };
        inline constexpr std::int16_t ILLUSTRATION_X{ 122 }, ILLUSTRATION_Y{ 268 };
        inline constexpr Bounds TIMED_SPEED{ .x = 456, .y = 396, .width = 268, .height = 56 };
        inline constexpr Bounds HOLD_SPEED{ .x = 456, .y = 396, .width = 544, .height = 56 };
        inline constexpr Bounds AXIS_CARD{ .x = 112, .y = 148, .width = 324, .height = 376 };
        inline constexpr Bounds CONTROLS_CARD{ .x = 448, .y = 148, .width = 560, .height = 376 };
        inline constexpr Bounds CURRENT_POSITION{ .x = 132, .y = 218, .width = 284, .height = 44 };
        inline constexpr Bounds SOFT_LIMITS{ .x = 128, .y = 458, .width = 292, .height = 28 };
        inline constexpr Bounds REFERENCE_STATUS{ .x = 128, .y = 488, .width = 292, .height = 28 };
        inline constexpr Bounds CONTROL_AXIS_JOG_PAGE{ .x = 456, .y = 156, .width = 176, .height = 56 };
        inline constexpr Bounds CONTROL_AXIS_POSITION{ .x = 640, .y = 156, .width = 176, .height = 56 };
        inline constexpr Bounds CONTROL_AXIS_DYNAMICS{ .x = 824, .y = 156, .width = 176, .height = 56 };
        inline constexpr Bounds CONTROL_STEP_MODE{ .x = 456, .y = 220, .width = 176, .height = 56 };
        inline constexpr Bounds CONTROL_TIMED{ .x = 640, .y = 220, .width = 176, .height = 56 };
        inline constexpr Bounds CONTROL_CONTINUOUS{ .x = 824, .y = 220, .width = 176, .height = 56 };
        inline constexpr Bounds BUTTON_MINUS{ .x = 520, .y = 284, .width = 104, .height = 104 };
        inline constexpr Bounds BUTTON_PLUS{ .x = 824, .y = 284, .width = 104, .height = 104 };
        inline constexpr Bounds CONTROL_STEP_SMALL{ .x = 456, .y = 396, .width = 176, .height = 56 };
        inline constexpr Bounds CONTROL_STEP_MEDIUM{ .x = 640, .y = 396, .width = 176, .height = 56 };
        inline constexpr Bounds CONTROL_STEP_LARGE{ .x = 824, .y = 396, .width = 176, .height = 56 };
        inline constexpr Bounds FIELD_DURATION_S{ .x = 736, .y = 396, .width = 264, .height = 56 };
        inline constexpr Bounds CONTROL_REFERENCE{ .x = 456, .y = 460, .width = 264, .height = 56 };
        inline constexpr Bounds CONTROL_AXIS_STOP{ .x = 736, .y = 460, .width = 264, .height = 56 };
        inline constexpr Bounds CONTROL_RELATIVE{ .x = 456, .y = 220, .width = 264, .height = 56 };
        inline constexpr Bounds CONTROL_ABSOLUTE{ .x = 736, .y = 220, .width = 264, .height = 56 };
        inline constexpr Bounds DESTINATION{ .x = 456, .y = 292, .width = 544, .height = 56 };
        inline constexpr Bounds SPEED{ .x = 456, .y = 364, .width = 544, .height = 56 };
        inline constexpr Bounds CONTROL_EXECUTE{ .x = 456, .y = 460, .width = 360, .height = 56 };
        inline constexpr Bounds CONTROL_AXIS_STOP_2{ .x = 832, .y = 460, .width = 168, .height = 56 };
        inline constexpr Bounds FIELD_ACCELERATION{ .x = 456, .y = 230, .width = 264, .height = 56 };
        inline constexpr Bounds FIELD_DECELERATION{ .x = 736, .y = 230, .width = 264, .height = 56 };
        inline constexpr Bounds FIELD_JERK{ .x = 456, .y = 310, .width = 264, .height = 56 };
        inline constexpr Bounds FIELD_TIMEOUT_S{ .x = 736, .y = 310, .width = 264, .height = 56 };
        inline constexpr Bounds DYNAMICS_UNITS{ .x = 468, .y = 390, .width = 520, .height = 32 };
        inline constexpr Bounds DYNAMICS_HELP{ .x = 468, .y = 432, .width = 520, .height = 72 };
    }
    namespace robot
    {
        inline constexpr Bounds CONTROLS_CARD{ .x = 112, .y = 148, .width = 896, .height = 376 };
        inline constexpr Bounds CONTROL_PREVIEW_JOINTS{ .x = 124, .y = 160, .width = 276, .height = 56 };
        inline constexpr Bounds CONTROL_PREVIEW_POINT{ .x = 412, .y = 160, .width = 276, .height = 56 };
        inline constexpr Bounds CONTROL_PREVIEW_LINE{ .x = 700, .y = 160, .width = 296, .height = 56 };
        inline constexpr Bounds CONTROL_ADVANCED{ .x = 700, .y = 160, .width = 296, .height = 56 };
        inline constexpr Bounds FIELD_ACCELERATION_FACTOR{ .x = 128, .y = 244, .width = 268, .height = 56 };
        inline constexpr Bounds FIELD_DECELERATION_FACTOR{ .x = 420, .y = 244, .width = 268, .height = 56 };
        inline constexpr Bounds FIELD_JERK_FACTOR{ .x = 712, .y = 244, .width = 280, .height = 56 };
        inline constexpr Bounds FIELD_TIMEOUT_S{ .x = 128, .y = 340, .width = 268, .height = 56 };
        inline constexpr Bounds DYNAMICS_HELP{ .x = 420, .y = 340, .width = 568, .height = 80 };
        inline constexpr Bounds FIRST_COORDINATE{ .x = 128, .y = 236, .width = 268, .height = 56 };
        inline constexpr Bounds SECOND_COORDINATE{ .x = 420, .y = 236, .width = 268, .height = 56 };
        inline constexpr Bounds THIRD_COORDINATE{ .x = 712, .y = 236, .width = 280, .height = 56 };
        inline constexpr Bounds FIELD_SPEED_FACTOR{ .x = 128, .y = 316, .width = 268, .height = 56 };
        inline constexpr Bounds CONTROL_RELATIVE{ .x = 420, .y = 316, .width = 268, .height = 56 };
        inline constexpr Bounds CONTROL_ABSOLUTE{ .x = 712, .y = 316, .width = 280, .height = 56 };
        inline constexpr Bounds CONTROL_REFERENCE{ .x = 128, .y = 460, .width = 268, .height = 56 };
        inline constexpr Bounds CONTROL_EXECUTE{ .x = 712, .y = 460, .width = 280, .height = 56 };
        inline constexpr Bounds PATH_HELP{ .x = 128, .y = 398, .width = 864, .height = 32 };
        inline constexpr Bounds CONTROL_ADVANCED_2{ .x = 420, .y = 316, .width = 268, .height = 56 };
        inline constexpr Bounds CONTROL_RUN_PREVIEW{ .x = 712, .y = 316, .width = 280, .height = 56 };
        inline constexpr Bounds LABEL_PREVIEW_ONLY_NO_MOTOR_MOVEMENT{ .x = 128,
                                                                      .y = 400,
                                                                      .width = 864,
                                                                      .height = 32 };
        inline constexpr Bounds CONTROL_TOGGLE_PREVIEWS{ .x = 128, .y = 460, .width = 268, .height = 56 };
    }
    namespace cartesian
    {
        inline constexpr RepeatedBounds POSITION_NAMES{
            .first = { .x = 132, .y = 226, .width = 32, .height = 36 },
            .dy = 64
        };
        inline constexpr RepeatedBounds POSITION_VALUES{
            .first = { .x = 172, .y = 222, .width = 244, .height = 44 },
            .dy = 64
        };
        inline constexpr GridBounds JOG_BUTTONS{ .first = { .x = 456, .y = 292, .width = 120, .height = 64 },
                                                 .columns = 2,
                                                 .dx = 128,
                                                 .dy = 72 };
        inline constexpr Bounds POSITION_CARD{ .x = 112, .y = 148, .width = 324, .height = 376 };
        inline constexpr Bounds CONTROLS_CARD{ .x = 448, .y = 148, .width = 560, .height = 376 };
        inline constexpr Bounds LABEL_ESTIMATED_TCP{ .x = 132, .y = 168, .width = 284, .height = 36 };
        inline constexpr Bounds LABEL_SOURCE{ .x = 128, .y = 420, .width = 292, .height = 40 };
        inline constexpr Bounds PATH_HELP{ .x = 128, .y = 468, .width = 292, .height = 48 };
        inline constexpr Bounds CONTROL_CARTESIAN_JOG_PAGE{ .x = 456, .y = 156, .width = 176, .height = 56 };
        inline constexpr Bounds CONTROL_CARTESIAN_POSITION{ .x = 640, .y = 156, .width = 176, .height = 56 };
        inline constexpr Bounds CONTROL_CARTESIAN_DYNAMICS{ .x = 824, .y = 156, .width = 176, .height = 56 };
        inline constexpr Bounds CONTROL_STEP_MODE{ .x = 456, .y = 220, .width = 176, .height = 56 };
        inline constexpr Bounds CONTROL_TIMED{ .x = 640, .y = 220, .width = 176, .height = 56 };
        inline constexpr Bounds CONTROL_CONTINUOUS{ .x = 824, .y = 220, .width = 176, .height = 56 };
        inline constexpr Bounds LABEL_STEP_SIZE{ .x = 728, .y = 284, .width = 264, .height = 32 };
        inline constexpr Bounds CONTROL_STEP_SMALL{ .x = 720, .y = 320, .width = 280, .height = 56 };
        inline constexpr Bounds CONTROL_STEP_MEDIUM{ .x = 720, .y = 384, .width = 280, .height = 56 };
        inline constexpr Bounds CONTROL_STEP_LARGE{ .x = 720, .y = 448, .width = 280, .height = 56 };
        inline constexpr Bounds FIELD_SPEED_FACTOR{ .x = 720, .y = 300, .width = 280, .height = 56 };
        inline constexpr Bounds FIELD_DURATION_S{ .x = 720, .y = 388, .width = 280, .height = 56 };
        inline constexpr Bounds LABEL_PRESS_AND_HOLD_N_RELEASE_TO_STOP{ .x = 728,
                                                                        .y = 388,
                                                                        .width = 264,
                                                                        .height = 72 };
        inline constexpr Bounds CONTROL_RELATIVE{ .x = 456, .y = 220, .width = 264, .height = 56 };
        inline constexpr Bounds CONTROL_ABSOLUTE{ .x = 736, .y = 220, .width = 264, .height = 56 };
        inline constexpr Bounds FIELD_X_MM{ .x = 456, .y = 292, .width = 168, .height = 56 };
        inline constexpr Bounds FIELD_Y_MM{ .x = 640, .y = 292, .width = 168, .height = 56 };
        inline constexpr Bounds FIELD_Z_MM{ .x = 824, .y = 292, .width = 176, .height = 56 };
        inline constexpr Bounds FIELD_SPEED_FACTOR_2{ .x = 456, .y = 364, .width = 544, .height = 56 };
        inline constexpr Bounds CONTROL_EXECUTE{ .x = 456, .y = 460, .width = 360, .height = 56 };
        inline constexpr Bounds CONTROL_GLOBAL_STOP{ .x = 832, .y = 460, .width = 168, .height = 56 };
        inline constexpr Bounds FIELD_ACCELERATION_FACTOR{ .x = 456, .y = 230, .width = 264, .height = 56 };
        inline constexpr Bounds FIELD_DECELERATION_FACTOR{ .x = 736, .y = 230, .width = 264, .height = 56 };
        inline constexpr Bounds FIELD_JERK_FACTOR{ .x = 456, .y = 302, .width = 264, .height = 56 };
        inline constexpr Bounds FIELD_TIMEOUT_S{ .x = 736, .y = 302, .width = 264, .height = 56 };
        inline constexpr Bounds FIELD_SPEED_FACTOR_3{ .x = 456, .y = 374, .width = 264, .height = 56 };
        inline constexpr Bounds DYNAMICS_HELP{ .x = 736, .y = 370, .width = 264, .height = 80 };
        inline constexpr Bounds LABEL_APPLIES_TO_THE_NEXT_MOVEMENT{ .x = 468,
                                                                    .y = 464,
                                                                    .width = 520,
                                                                    .height = 40 };
    }
    namespace motor
    {
        inline constexpr Bounds CONTROLS_CARD{ .x = 112, .y = 148, .width = 896, .height = 376 };
        inline constexpr Bounds CONTROL_MOTOR_MOVE_TAB{ .x = 424, .y = 160, .width = 180, .height = 56 };
        inline constexpr Bounds CONTROL_MOTOR_HOME_TAB{ .x = 616, .y = 160, .width = 180, .height = 56 };
        inline constexpr Bounds CONTROL_MOTOR_DEFAULTS_TAB{ .x = 808, .y = 160, .width = 188, .height = 56 };
        inline constexpr Bounds MOTOR_STATE{ .x = 132, .y = 228, .width = 864, .height = 32 };
        inline constexpr Bounds FIELD_ACCEL_S{ .x = 128, .y = 274, .width = 268, .height = 56 };
        inline constexpr Bounds FIELD_DECEL_S{ .x = 420, .y = 274, .width = 268, .height = 56 };
        inline constexpr Bounds FIELD_JERK_S{ .x = 712, .y = 274, .width = 280, .height = 56 };
        inline constexpr Bounds FIELD_TIMEOUT_S{ .x = 128, .y = 354, .width = 268, .height = 56 };
        inline constexpr Bounds LABEL_APPLIES_TO_THE_NEXT_MOTOR_MOVE{ .x = 420,
                                                                      .y = 374,
                                                                      .width = 568,
                                                                      .height = 32 };
        inline constexpr Bounds CONTROL_ADVANCED{ .x = 128, .y = 454, .width = 560, .height = 56 };
        inline constexpr Bounds CONTROL_ABORT{ .x = 712, .y = 454, .width = 280, .height = 56 };
        inline constexpr Bounds FIELD_SHAFT_DEGREES{ .x = 128, .y = 274, .width = 268, .height = 56 };
        inline constexpr Bounds FIELD_SPEED_RPM{ .x = 420, .y = 274, .width = 268, .height = 56 };
        inline constexpr Bounds CONTROL_NEXT_BUFFER{ .x = 712, .y = 274, .width = 280, .height = 56 };
        inline constexpr Bounds CONTROL_RELATIVE{ .x = 128, .y = 346, .width = 268, .height = 56 };
        inline constexpr Bounds CONTROL_ABSOLUTE{ .x = 420, .y = 346, .width = 268, .height = 56 };
        inline constexpr Bounds CONTROL_ACTIVE_SPEED{ .x = 712, .y = 346, .width = 280, .height = 56 };
        inline constexpr Bounds CONTROL_ADVANCED_2{ .x = 128, .y = 454, .width = 268, .height = 56 };
        inline constexpr Bounds CONTROL_EXECUTE{ .x = 420, .y = 454, .width = 268, .height = 56 };
        inline constexpr Bounds FIELD_SEEK_RPM{ .x = 128, .y = 284, .width = 268, .height = 56 };
        inline constexpr Bounds FIELD_LATCH_RPM{ .x = 420, .y = 284, .width = 268, .height = 56 };
        inline constexpr Bounds FIELD_TIMEOUT_S_2{ .x = 712, .y = 284, .width = 280, .height = 56 };
        inline constexpr Bounds HOMING_HELP{ .x = 132, .y = 378, .width = 856, .height = 32 };
        inline constexpr Bounds CONTROL_REFERENCE{ .x = 128, .y = 454, .width = 560, .height = 56 };
        inline constexpr Bounds FIELD_ACCEL_S_2{ .x = 128, .y = 284, .width = 268, .height = 56 };
        inline constexpr Bounds FIELD_DECEL_S_2{ .x = 420, .y = 284, .width = 268, .height = 56 };
        inline constexpr Bounds FIELD_JERK_S_2{ .x = 712, .y = 284, .width = 280, .height = 56 };
        inline constexpr Bounds FIELD_TIMEOUT_S_3{ .x = 128, .y = 364, .width = 268, .height = 56 };
        inline constexpr Bounds DEFAULTS_HELP{ .x = 420, .y = 378, .width = 568, .height = 32 };
        inline constexpr Bounds CONTROL_APPLY_DEFAULTS{ .x = 128, .y = 454, .width = 560, .height = 56 };
    }
    namespace maintenance
    {
        inline constexpr RepeatedBounds REFERENCE_LAMPS{
            .first = { .x = 132, .y = 234, .width = 0, .height = 0 },
            .dx = 0,
            .dy = 80
        };
        inline constexpr RepeatedBounds REFERENCE_LABELS{
            .first = { .x = 156, .y = 224, .width = 216, .height = 32 },
            .dx = 0,
            .dy = 80
        };
        inline constexpr RepeatedBounds REFERENCE_VALUES{
            .first = { .x = 372, .y = 224, .width = 160, .height = 0 },
            .dx = 0,
            .dy = 80
        };
        inline constexpr RepeatedBounds ALARM_LABELS{
            .first = { .x = 132, .y = 254, .width = 380, .height = 28 },
            .dx = 0,
            .dy = 80
        };
        inline constexpr RepeatedBounds DRIVER_BUTTONS{
            .first = { .x = 124, .y = 390, .width = 280, .height = 56 },
            .dx = 292,
            .dy = 0
        };
        inline constexpr RepeatedBounds JOB_CARDS{
            .first = { .x = 128, .y = 160, .width = 864, .height = 80 },
            .dx = 0,
            .dy = 84
        };
        inline constexpr RepeatedBounds JOB_HEADINGS{
            .first = { .x = 136, .y = 168, .width = 680, .height = 32 },
            .dx = 0,
            .dy = 84
        };
        inline constexpr RepeatedBounds JOB_DETAILS{
            .first = { .x = 136, .y = 200, .width = 680, .height = 42 },
            .dx = 0,
            .dy = 84
        };
        inline constexpr RepeatedBounds JOB_CANCELS{
            .first = { .x = 844, .y = 174, .width = 144, .height = 56 },
            .dx = 0,
            .dy = 84
        };
        inline constexpr RepeatedBounds MESSAGE_CARDS{
            .first = { .x = 128, .y = 240, .width = 864, .height = 46 },
            .dx = 0,
            .dy = 50
        };
        inline constexpr RepeatedBounds MESSAGE_LEVELS{
            .first = { .x = 140, .y = 242, .width = 72, .height = 42 },
            .dx = 0,
            .dy = 50
        };
        inline constexpr RepeatedBounds MESSAGE_TEXTS{
            .first = { .x = 220, .y = 242, .width = 760, .height = 42 },
            .dx = 0,
            .dy = 50
        };
        inline constexpr Bounds ENABLE_LAMP{ .x = 584, .y = 234 };
        inline constexpr Bounds DRIVER_TABLE{ .x = 120, .y = 156, .width = 880, .height = 224 };
        inline constexpr Bounds CONTROL_IODIGITAL{ .x = 112, .y = 84, .width = 440, .height = 56 };
        inline constexpr Bounds CONTROL_IODRIVERS{ .x = 564, .y = 84, .width = 444, .height = 56 };
        inline constexpr Bounds INPUTS_CARD{ .x = 112, .y = 148, .width = 440, .height = 376 };
        inline constexpr Bounds OUTPUTS_CARD{ .x = 564, .y = 148, .width = 444, .height = 376 };
        inline constexpr Bounds LABEL_INPUTS{ .x = 132, .y = 168, .width = 400, .height = 36 };
        inline constexpr Bounds LABEL_OUTPUTS{ .x = 584, .y = 168, .width = 400, .height = 36 };
        inline constexpr Bounds LABEL_ENABLE_COMMAND{ .x = 608, .y = 224, .width = 226, .height = 32 };
        inline constexpr Bounds VALUE_USABLE_UNKNOWN_S_ENABLED_ON_OFF{ .x = 844,
                                                                       .y = 224,
                                                                       .width = 144,
                                                                       .height = 0 };
        inline constexpr Bounds LABEL_T{ .x = 132, .y = 464, .width = 860, .height = 48 };
        inline constexpr Bounds DETAILS_CARD{ .x = 112, .y = 84, .width = 896, .height = 440 };
        inline constexpr Bounds CONTROL_TOGGLE_PREVIEWS{ .x = 128, .y = 96, .width = 292, .height = 56 };
        inline constexpr Bounds CONTROL_PREV_JOBS{ .x = 712, .y = 96, .width = 132, .height = 56 };
        inline constexpr Bounds CONTROL_NEXT_JOBS{ .x = 856, .y = 96, .width = 136, .height = 56 };
        inline constexpr Bounds LABEL_NO_JOBS_YET{ .x = 160, .y = 244, .width = 800, .height = 44 };
        inline constexpr Bounds JOBS_HELP{ .x = 160, .y = 306, .width = 800, .height = 32 };
        inline constexpr Bounds LABEL_S_CONDITION_DATA{ .x = 136, .y = 104, .width = 856, .height = 68 };
        inline constexpr Bounds CONTROL_TOGGLE_INFO{ .x = 768, .y = 176, .width = 224, .height = 56 };
        inline constexpr Bounds LABEL_RECENT_MESSAGES{ .x = 136, .y = 188, .width = 600, .height = 32 };
        inline constexpr Bounds EMPTY_MESSAGES{ .x = 136, .y = 290, .width = 832, .height = 32 };
        inline constexpr Bounds CONTROL_ACKNOWLEDGE{ .x = 128, .y = 460, .width = 352, .height = 56 };
        inline constexpr Bounds FAULTS_HELP{ .x = 500, .y = 468, .width = 488, .height = 48 };
        inline constexpr Bounds EMPTY_PAGE_TITLE{ .x = 160, .y = 228, .width = 800, .height = 48 };
        inline constexpr Bounds LABEL_NOT_IMPLEMENTED_YET{ .x = 160, .y = 296, .width = 800, .height = 32 };
        inline constexpr Bounds EMPTY_PAGE_HELP{ .x = 160, .y = 342, .width = 800, .height = 32 };
    }
    namespace settings
    {
        inline constexpr Bounds BRIGHTNESS_TRACK{ .x = 136, .y = 250, .width = 840, .height = 56 };
        inline constexpr Bounds CONFIGURATION_TABLE{ .x = 128, .y = 270, .width = 864, .height = 164 };
        inline constexpr Bounds DIAGNOSTIC_TABLE{ .x = 124, .y = 220, .width = 872, .height = 238 };
        inline constexpr Bounds CONTROL_SETTINGS_GENERAL{ .x = 112, .y = 84, .width = 288, .height = 56 };
        inline constexpr Bounds CONTROL_SETTINGS_ROBOT{ .x = 416, .y = 84, .width = 288, .height = 56 };
        inline constexpr Bounds CONTROL_SETTINGS_DIAGNOSTICS{ .x = 720, .y = 84, .width = 288, .height = 56 };
        inline constexpr Bounds CONTENTS_CARD{ .x = 112, .y = 148, .width = 896, .height = 376 };
        inline constexpr Bounds LABEL_SCREEN_BRIGHTNESS{ .x = 136, .y = 184, .width = 500, .height = 36 };
        inline constexpr Bounds BRIGHTNESS_VALUE{ .x = 804, .y = 184, .width = 180, .height = 0 };
        inline constexpr Bounds BRIGHTNESS_HELP{ .x = 136, .y = 344, .width = 832, .height = 32 };
        inline constexpr Bounds CONTROL_PRESETS{ .x = 136, .y = 444, .width = 300, .height = 56 };
        inline constexpr Bounds CONFIGURATION_STATE{ .x = 136, .y = 176, .width = 832, .height = 40 };
        inline constexpr Bounds CONFIGURATION_ID{ .x = 136, .y = 228, .width = 832, .height = 32 };
        inline constexpr Bounds CONTROL_MOTORS{ .x = 136, .y = 444, .width = 300, .height = 56 };
        inline constexpr Bounds LABEL_CHANGES_REQUIRE_AN_IDLE_MOTOR{ .x = 460,
                                                                     .y = 460,
                                                                     .width = 500,
                                                                     .height = 32 };
        inline constexpr Bounds CONTROL_DIAGNOSTIC_UI{ .x = 124, .y = 156, .width = 92, .height = 56 };
        inline constexpr Bounds CONTROL_DIAGNOSTIC_IO{ .x = 224, .y = 156, .width = 92, .height = 56 };
        inline constexpr Bounds CONTROL_DIAGNOSTIC_HEAP{ .x = 332, .y = 156, .width = 168, .height = 56 };
        inline constexpr Bounds CONTROL_DIAGNOSTIC_STACKS{ .x = 508, .y = 156, .width = 168, .height = 56 };
        inline constexpr Bounds CONTROL_DIAGNOSTIC_GRAPHICS{ .x = 684, .y = 156, .width = 168, .height = 56 };
        inline constexpr Bounds CONTROL_DIAGNOSTIC_REFRESH{ .x = 860, .y = 156, .width = 136, .height = 56 };
        inline constexpr Bounds LABEL_UI_GRAPHICS_LIVE_COUNTERS{ .x = 136,
                                                                 .y = 472,
                                                                 .width = 544,
                                                                 .height = 32 };
        inline constexpr Bounds CONTROL_DIAGNOSTIC_PREVIOUS{ .x = 700, .y = 464, .width = 140, .height = 56 };
        inline constexpr Bounds CONTROL_DIAGNOSTIC_NEXT{ .x = 852, .y = 464, .width = 144, .height = 56 };
        inline constexpr Bounds LABEL_RETRIEVING_DIAGNOSTIC_SNAPSHOT{ .x = 136,
                                                                      .y = 248,
                                                                      .width = 832,
                                                                      .height = 40 };
        inline constexpr Bounds SNAPSHOT_ERROR{ .x = 136, .y = 248, .width = 832, .height = 72 };
        inline constexpr Bounds LABEL_NOTE{ .x = 136, .y = 472, .width = 544, .height = 44 };
    }
}
