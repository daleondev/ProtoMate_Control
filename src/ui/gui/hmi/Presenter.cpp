#include "Presenter.hpp"
#include "NumericField.hpp"
#include "Text.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
namespace ui::gui::hmi
{
    namespace
    {
        constexpr unsigned MESSAGE_BYTES{ 160 }, DIAGNOSTIC_PAGE_ROWS{ 6 }, DIAGNOSTIC_LAST_PAGE{ 30 },
          BUFFER_MODES{ 6 };
        constexpr double RAW_DEFAULT_SPEED{ 10 }, SMALL_STEP{ .1 }, LARGE_STEP{ 10 },
          DEFAULT_JOG_FACTOR{ .1 };
        Kind reference_kind(Page page)
        {
            if (page == Page::Motors) {
                return Kind::MotorHome;
            }
            if (page == Page::Axes) {
                return Kind::AxisHome;
            }
            return Kind::RobotHome;
        }
        Kind preview_kind(unsigned coordinates)
        {
            if (coordinates == 0) {
                return Kind::PreviewJoint;
            }
            if (coordinates == 1) {
                return Kind::PreviewPoint;
            }
            return Kind::PreviewLine;
        }
        Kind move_kind(const Model& model)
        {
            if (model.page == Page::Motors) {
                return model.absolute ? Kind::MotorAbsolute : Kind::MotorRelative;
            }
            if (model.page == Page::Axes) {
                return model.absolute ? Kind::AxisAbsolute : Kind::AxisRelative;
            }
            if (model.page == Page::Cartesian || model.coordinates == 1) {
                return model.absolute ? Kind::PointAbsolute : Kind::PointRelative;
            }
            return model.absolute ? Kind::JointAbsolute : Kind::JointRelative;
        }
    }
    void Presenter::tick()
    {
        ++m_model.ticks;
        if (!m_model.backend) {
            return;
        }
        m_model.backend->read(m_model.state);
        if (m_model.state.outcome.operation != m_model.last_operation) {
            m_model.last_operation = m_model.state.outcome.operation;
            m_model.report(m_model.state.outcome.message.data(), m_model.state.outcome.severity);
        }
        if (m_model.watched_job && !m_model.previews) {
            m_model.job_count = m_model.backend->jobs(0, m_model.jobs, false);
            for (const auto& job : std::span(m_model.jobs).first(m_model.job_count)) {
                if (job.id == m_model.watched_job && job.state != m_model.watched_state) {
                    m_model.watched_state = job.state;
                    std::array<char, MESSAGE_BYTES> line{};
                    format(line, "Job #{}: {} · {:.90}", job.id, job.status.data(), job.detail.data());
                    m_model.report(line.data(), job.severity);
                    if (job.terminal) {
                        m_model.watched_job = 0;
                    }
                }
            }
        }
        if (m_model.page == Page::Jobs) {
            m_model.job_count = m_model.backend->jobs(m_model.job_offset, m_model.jobs, m_model.previews);
        }
    }
    void Presenter::navigate(Page page)
    {
        if (m_model.backend) {
            m_model.backend->release();
        }
        if (m_model.page != page) {
            const auto brightness{ m_model.values[fields::BRIGHTNESS] };
            m_model.values = fields::DEFAULTS;
            m_model.values[fields::BRIGHTNESS] = brightness;
            if (page == Page::Motors) {
                m_model.values[fields::SPEED] = RAW_DEFAULT_SPEED;
                const auto& motor{ m_model.state.motor(m_model.axis) };
                m_model.values[fields::ACCELERATION] = motor.acceleration;
                m_model.values[fields::DECELERATION] = motor.deceleration;
                m_model.values[fields::JERK] = motor.jerk;
            }
        }
        m_model.page = page;
        m_model.keypad = false;
        m_model.confirmation = false;
        m_model.coordinates = 0;
        m_model.more = m_model.advanced = false;
        m_model.axis_tab = m_model.motor_tab = m_model.cartesian_tab = 0;
        m_model.job_offset = 0;
        m_pending.changed = true;
    }
    Command Presenter::command(Kind kind) const
    {
        const auto& v{ m_model.values };
        return { .kind = kind,
                 .axis = m_model.axis,
                 .position = { v[fields::POSITION_1], v[fields::POSITION_2], v[fields::POSITION_3] },
                 .speed = v[fields::SPEED],
                 .acceleration = v[fields::ACCELERATION],
                 .deceleration = v[fields::DECELERATION],
                 .jerk = v[fields::JERK],
                 .timeout = v[fields::TIMEOUT],
                 .duration = v[fields::DURATION],
                 .latch = v[fields::LATCH_SPEED],
                 .buffer = m_model.buffer,
                 .continuous =
                   (m_model.page == Page::Cartesian ? m_model.cartesian_jog_mode : m_model.jog_mode) == 2 };
    }
    void Presenter::send(Command c)
    {
        if (!m_model.backend) {
            m_model.report("Motion backend unavailable", Severity::Error);
            return;
        }
        if ((c.kind == Kind::MotorRelative || c.kind == Kind::MotorAbsolute || c.kind == Kind::MotorHome) &&
            !c.confirmed && c.buffer == 0 && m_model.state.motor(c.axis).owner) {
            m_confirmation = c;
            m_model.confirmation = true;
            m_pending.changed = true;
            return;
        }
        const auto outcome{ m_model.backend->submit(c) };
        m_model.report(outcome.message.data(), outcome.accepted ? outcome.severity : Severity::Warning);
        if (outcome.accepted && outcome.job && m_model.page != Page::Preview) {
            m_model.watched_job = outcome.job;
            m_model.watched_state = ~0U;
        }
    }
    void Presenter::requestDiagnostics()
    {
        if (m_model.diagnostic_tab == 2) {
            return;
        }
        Kind kind{ m_model.diagnostic_io ? Kind::MemoryIO : Kind::MemoryUI };
        if (m_model.diagnostic_tab == 1) {
            kind = m_model.diagnostic_io ? Kind::MemoryIOStack : Kind::MemoryUIStack;
        }
        send(command(kind));
    }
    void Presenter::key(char key)
    {
        if (!m_model.keypad) {
            return;
        }
        if (key >= '0' && key <= '9') {
            act(Key0 + unsigned(key - '0'), true);
        }
        else if (key == '.' || key == ',') {
            act(KeyDot, true);
        }
        else if ((key == '-' && m_model.number[0] != '-') || (key == '+' && m_model.number[0] == '-')) {
            act(KeySign, true);
        }
        else if (key == '\b') {
            act(KeyBack, true);
        }
        else if (key == '\n') {
            act(KeyApply, true);
        }
        else if (key == '\x1b') {
            act(KeyCancel, true);
        }
        else if (key == '\x7f') {
            m_model.number.fill(0);
            m_pending.changed = true;
        }
    }
    void Presenter::editNumber(Action a)
    {
        if (!m_model.keypad) {
            return;
        }
        const auto rules{ NumericField::forModel(m_model) };
        auto& text{ m_model.number };
        const auto n{ std::strlen(text.data()) };
        if (a <= Key9 && n + 1 < text.size()) {
            text[n] = char('0' + (a - Key0));
            text[n + 1] = 0;
        }
        else if (a == KeyDot && !std::strchr(text.data(), '.') && n + 1 < text.size()) {
            text[n] = '.';
            text[n + 1] = 0;
        }
        else if (a == KeySign && rules.signedValue()) {
            if (text[0] == '-') {
                std::memmove(text.data(), text.data() + 1, n);
            }
            else if (n + 1 < text.size()) {
                std::memmove(text.data() + 1, text.data(), n + 1);
                text[0] = '-';
            }
        }
        else if (a == KeyBack && n) {
            text[n - 1] = 0;
        }
        else if (a == KeyCancel) {
            text.fill(0);
            m_model.keypad = false;
        }
        else if (a == KeyApply) {
            if (const auto value{ rules.parse(text.data()) }) {
                m_model.values[m_model.field] = *value;
                m_model.keypad = false;
                if (m_model.field == fields::BRIGHTNESS) {
                    m_pending.brightness_changed = true;
                }
            }
            else {
                m_model.report("Invalid value: check the field range", Severity::Warning);
            }
        }
        m_pending.changed = true;
    }
    void Presenter::act(Action a, bool pressed)
    {
        if (!pressed) {
            if (m_model.backend) {
                m_model.backend->release();
            }
            return;
        }
        ++m_model.interactions;
        if (a >= Field0 && a <= FieldLast) {
            if (m_model.backend) {
                m_model.backend->release();
            }
            m_model.field = a - Field0;
            m_model.keypad = true;
            m_model.number.fill(0);
            if (++m_model.editor_revision == 0) {
                ++m_model.editor_revision;
            }
            m_pending.changed = true;
            return;
        }
        if (a >= Key0 && a <= KeyCancel) {
            editNumber(a);
            return;
        }
        static_cast<void>(navigation(a) || movementModes(a) || diagnosticActions(a) || motionActions(a) ||
                          historyActions(a));
    }
    bool Presenter::navigation(Action a)
    {
        switch (a) {
            case Home:
                navigate(Page::Home);
                break;
            case Move:
            case Axes:
                navigate(Page::Axes);
                break;
            case Programs:
                navigate(Page::Programs);
                break;
            case IO:
                navigate(Page::IO);
                break;
            case Settings:
                navigate(Page::Settings);
                if (m_model.settings_tab == 2) {
                    requestDiagnostics();
                }
                break;
            case Jobs:
                navigate(Page::Jobs);
                break;
            case Alarms:
                navigate(Page::Alarms);
                break;
            case Robot:
                navigate(Page::Robot);
                break;
            case Cartesian:
                navigate(Page::Cartesian);
                break;
            case Preview:
                navigate(Page::Preview);
                break;
            case Motors:
                navigate(Page::Motors);
                break;
            case Presets:
                navigate(Page::Presets);
                break;
            case Axis1:
            case Axis2:
            case Axis3:
                if (m_model.backend) {
                    m_model.backend->release();
                }
                m_model.axis = a - Axis1;
                if (m_model.page == Page::Home) {
                    navigate(Page::Axes);
                }
                m_pending.changed = true;
                break;
            case More:
                if (m_model.backend) {
                    m_model.backend->release();
                }
                m_model.more = !m_model.more;
                m_pending.changed = true;
                break;
            case Advanced:
                if (m_model.backend) {
                    m_model.backend->release();
                }
                m_model.advanced = !m_model.advanced;
                m_pending.changed = true;
                break;
            default:
                return false;
        }
        return true;
    }
    void Presenter::releaseHeld() const
    {
        if (m_model.backend) {
            m_model.backend->release();
        }
    }
    bool Presenter::movementModes(Action a)
    {
        switch (a) {
            case StepSmall:
                (m_model.page == Page::Cartesian ? m_model.cartesian_step : m_model.step) = SMALL_STEP;
                m_pending.changed = true;
                break;
            case StepMedium:
                (m_model.page == Page::Cartesian ? m_model.cartesian_step : m_model.step) = 1;
                m_pending.changed = true;
                break;
            case StepLarge:
                (m_model.page == Page::Cartesian ? m_model.cartesian_step : m_model.step) = LARGE_STEP;
                m_pending.changed = true;
                break;
            case Relative:
                m_model.absolute = false;
                m_pending.changed = true;
                break;
            case Absolute:
                m_model.absolute = true;
                m_pending.changed = true;
                break;
            case Timed:
            case Continuous:
                releaseHeld();
                (m_model.page == Page::Cartesian ? m_model.cartesian_jog_mode : m_model.jog_mode) =
                  a == Continuous ? 2 : 1;
                m_pending.changed = true;
                break;
            case AxisPosition:
                releaseHeld();
                m_model.axis_tab = 1;
                m_pending.changed = true;
                break;
            case CartesianJogPage:
            case CartesianPosition:
            case CartesianDynamics:
                releaseHeld();
                m_model.cartesian_tab = a - CartesianJogPage;
                m_pending.changed = true;
                break;
            case StepMode:
                releaseHeld();
                (m_model.page == Page::Cartesian ? m_model.cartesian_jog_mode : m_model.jog_mode) = 0;
                m_pending.changed = true;
                break;
            case MotorMoveTab:
            case MotorHomeTab:
            case MotorDefaultsTab:
                releaseHeld();
                m_model.motor_tab = a - MotorMoveTab;
                m_pending.changed = true;
                break;
            case PreviewJoints:
            case PreviewPoint:
            case PreviewLine:
                m_model.coordinates = a - PreviewJoints;
                m_pending.changed = true;
                break;
            case NextBuffer:
                m_model.buffer = (m_model.buffer + 1) % BUFFER_MODES;
                m_pending.changed = true;
                break;
            case AxisDynamics:
                releaseHeld();
                m_model.axis_tab = 2;
                m_pending.changed = true;
                break;
            case AxisJogPage:
                releaseHeld();
                m_model.axis_tab = 0;
                m_pending.changed = true;
                break;
            default:
                return false;
        }
        return true;
    }
    bool Presenter::diagnosticActions(Action a)
    {
        switch (a) {
            case SettingsGeneral:
            case SettingsRobot:
            case SettingsDiagnostics:
                m_model.settings_tab = a - SettingsGeneral;
                if (a == SettingsDiagnostics) {
                    requestDiagnostics();
                }
                m_pending.changed = true;
                break;
            case IODigital:
            case IODrivers:
                m_model.io_tab = a - IODigital;
                m_pending.changed = true;
                break;
            case Driver0:
            case Driver1:
            case Driver2:
                m_model.driver_detail = a - Driver0;
                m_pending.changed = true;
                break;
            case DiagnosticUI:
            case DiagnosticIO:
                m_model.diagnostic_io = a == DiagnosticIO;
                m_model.diagnostic_offset = 0;
                requestDiagnostics();
                m_pending.changed = true;
                break;
            case DiagnosticHeap:
            case DiagnosticStacks:
            case DiagnosticGraphics:
                m_model.diagnostic_tab = a - DiagnosticHeap;
                if (a == DiagnosticGraphics) {
                    m_model.diagnostic_io = false;
                }
                m_model.diagnostic_offset = 0;
                requestDiagnostics();
                m_pending.changed = true;
                break;
            case DiagnosticRefresh:
                requestDiagnostics();
                m_pending.changed = true;
                break;
            case DiagnosticPrevious:
                m_model.diagnostic_offset = m_model.diagnostic_offset >= DIAGNOSTIC_PAGE_ROWS
                                              ? m_model.diagnostic_offset - DIAGNOSTIC_PAGE_ROWS
                                              : 0;
                m_pending.changed = true;
                break;
            case DiagnosticNext:
                if (m_model.diagnostic_offset < DIAGNOSTIC_LAST_PAGE) {
                    m_model.diagnostic_offset += DIAGNOSTIC_PAGE_ROWS;
                }
                m_pending.changed = true;
                break;
            case MemoryUIStack:
                send(command(Kind::MemoryUIStack));
                break;
            case MemoryIOStack:
                send(command(Kind::MemoryIOStack));
                break;
            case MemoryUI:
                send(command(Kind::MemoryUI));
                break;
            case MemoryIO:
                send(command(Kind::MemoryIO));
                break;
            default:
                return false;
        }
        return true;
    }
    bool Presenter::motionActions(Action a)
    {
        switch (a) {
            case Initialize:
                send(command(Kind::Initialize));
                break;
            case Enable:
                send(command(Kind::Enable));
                break;
            case Disable:
                send(command(Kind::Disable));
                break;
            case GlobalStop:
                if (m_model.backend) {
                    m_model.backend->release();
                }
                send(command(Kind::Stop));
                break;
            case Abort:
                send(command(Kind::MotorAbort));
                break;
            case Reference:
                send(command(reference_kind(m_model.page)));
                break;
            case Minus:
            case Plus: {
                auto c{ command(Kind::AxisRelative) };
                c.position[0] = (a == Minus ? -1 : 1) * m_model.step;
                send(c);
                break;
            }
            case JogMinus:
            case JogPlus:
                jogAxis(a);
                break;
            case Execute:
                executeMove(a);
                break;
            case XMinus:
            case XPlus:
            case YMinus:
            case YPlus:
            case ZMinus:
            case ZPlus:
                jogCartesian(a);
                break;
            case RunPreview: {
                auto c{ command(preview_kind(m_model.coordinates)) };
                c.speed = m_model.values[fields::SPEED_FACTOR];
                send(c);
                break;
            }
            case ApplyDefaults:
                send(command(Kind::MotorDefaults));
                break;
            case ActiveSpeed:
                send(command(Kind::MotorSpeed));
                break;
            case AxisStop:
                if (m_model.backend) {
                    m_model.backend->release();
                }
                send(command(Kind::AxisStop));
                break;
            case Confirm:
                m_model.confirmation = false;
                m_confirmation.confirmed = true;
                send(m_confirmation);
                m_pending.changed = true;
                break;
            case Reject:
                m_model.confirmation = false;
                m_pending.changed = true;
                break;
            default:
                return false;
        }
        return true;
    }
    bool Presenter::historyActions(Action a)
    {
        switch (a) {
            case ZoomIn:
                m_pending.camera_in = true;
                break;
            case ZoomOut:
                m_pending.camera_out = true;
                break;
            case ResetView:
                m_pending.camera_reset = true;
                break;
            case PrevJobs:
                m_model.job_offset = m_model.job_offset >= 4 ? m_model.job_offset - 4 : 0;
                m_pending.changed = true;
                break;
            case NextJobs:
                if (m_model.job_count == 4) {
                    m_model.job_offset += 4;
                }
                m_pending.changed = true;
                break;
            case TogglePreviews:
                if (m_model.page == Page::Preview) {
                    navigate(Page::Jobs);
                    m_model.previews = true;
                    break;
                }
                m_model.previews = !m_model.previews;
                m_model.job_offset = 0;
                m_pending.changed = true;
                break;
            case Cancel0:
            case Cancel1:
            case Cancel2:
            case Cancel3: {
                const auto i{ a - Cancel0 };
                if (i < m_model.job_count) {
                    auto c{ command(m_model.previews ? Kind::CancelPreview : Kind::Cancel) };
                    c.job = m_model.jobs[i].id;
                    send(c);
                }
                break;
            }
            case ToggleInfo:
                m_model.show_info = !m_model.show_info;
                m_pending.changed = true;
                break;
            case Acknowledge:
                m_model.message_count = 0;
                m_model.notice.fill(0);
                m_model.report("Messages acknowledged locally; active faults remain");
                m_pending.changed = true;
                break;
            default:
                return false;
        }
        return true;
    }
    void Presenter::jogAxis(Action a)
    {
        auto c{ command(Kind::AxisJog) };
        c.speed =
          (a == JogMinus ? -1 : 1) * (m_model.values[fields::SPEED] > 0
                                        ? m_model.values[fields::SPEED]
                                        : DEFAULT_JOG_FACTOR * m_model.state.axes[m_model.axis].max_speed);
        if (c.continuous && m_model.backend) {
            const auto result{ m_model.backend->hold(c,
                                                     { .x = m_pending.hold_x,
                                                       .y = m_pending.hold_y,
                                                       .width = m_pending.hold_width,
                                                       .height = m_pending.hold_height }) };
            m_model.report(result.message.data(), result.accepted ? result.severity : Severity::Warning);
        }
        else {
            send(c);
        }
    }
    void Presenter::executeMove(Action /*unused*/)
    {
        const Kind k{ move_kind(m_model) };
        auto c{ command(k) };
        if (m_model.page == Page::Robot || m_model.page == Page::Cartesian) {
            c.speed = m_model.values[fields::SPEED_FACTOR];
            c.acceleration = c.acceleration == 0 ? 1 : c.acceleration;
            c.deceleration = c.deceleration == 0 ? 1 : c.deceleration;
            c.jerk = c.jerk == 0 ? 1 : c.jerk;
        }
        send(c);
    }
    void Presenter::jogCartesian(Action a)
    {
        const bool step{ m_model.cartesian_jog_mode == 0 };
        auto c{ command(step ? Kind::PointRelative : Kind::PointJog) };
        c.position = {};
        // Jog carries a direction. Only Step exposes a distance.
        c.position[(a - XMinus) / 2] =
          ((a - XMinus) % 2 == 0 ? -1 : 1) * (step ? m_model.cartesian_step : 1.);
        c.speed = m_model.values[fields::SPEED_FACTOR];
        c.acceleration = c.acceleration == 0 ? 1 : c.acceleration;
        c.deceleration = c.deceleration == 0 ? 1 : c.deceleration;
        c.jerk = c.jerk == 0 ? 1 : c.jerk;
        if (m_model.cartesian_jog_mode == 2 && m_model.backend) {
            const auto result{ m_model.backend->hold(c,
                                                     { .x = m_pending.hold_x,
                                                       .y = m_pending.hold_y,
                                                       .width = m_pending.hold_width,
                                                       .height = m_pending.hold_height }) };
            m_model.report(result.message.data(), result.accepted ? result.severity : Severity::Warning);
        }
        else {
            send(c);
        }
    }

}
