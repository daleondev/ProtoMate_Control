#include "Presenter.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
namespace ui::gui::hmi
{
    void Presenter::tick()
    {
        ++m_model.ticks;
        if (!m_model.backend)
            return;
        m_model.backend->read(m_model.state);
        if (m_model.state.outcome.operation != m_model.lastOperation) {
            m_model.lastOperation = m_model.state.outcome.operation;
            m_model.report(m_model.state.outcome.message.data());
        }
        if (m_model.watchedJob && !m_model.previews) {
            m_model.jobCount = m_model.backend->jobs(0, m_model.jobs, false);
            for (const auto& job : std::span(m_model.jobs).first(m_model.jobCount))
                if (job.id == m_model.watchedJob && job.state != m_model.watchedState) {
                    m_model.watchedState = job.state;
                    char line[160];
                    std::snprintf(line,
                                  sizeof(line),
                                  "Job #%llu: %s · %.90s",
                                  (unsigned long long)job.id,
                                  job.status.data(),
                                  job.detail.data());
                    m_model.report(line);
                    if (job.terminal)
                        m_model.watchedJob = 0;
                }
        }
        if (m_model.page == Page::Jobs)
            m_model.jobCount = m_model.backend->jobs(m_model.jobOffset, m_model.jobs, m_model.previews);
    }
    void Presenter::navigate(Page page)
    {
        if (m_model.backend)
            m_model.backend->release();
        if (m_model.page != page) {
            m_model.values = { 0, 0, 0, 0, 0, 0, 0, 0, 1, .5, .25, m_model.values[11] };
            if (page == Page::Motors) {
                m_model.values[3] = 10;
                const auto& motor = m_model.state.motor(m_model.axis);
                m_model.values[4] = motor.acceleration;
                m_model.values[5] = motor.deceleration;
                m_model.values[6] = motor.jerk;
            }
        }
        m_model.page = page;
        m_model.keypad = false;
        m_model.confirmation = false;
        m_model.coordinates = 0;
        m_model.more = m_model.advanced = false;
        m_model.axisTab = m_model.motorTab = 0;
        m_model.jobOffset = 0;
        changed = true;
    }
    Command Presenter::command(Kind kind) const
    {
        const auto& v = m_model.values;
        return { .kind = kind,
                 .axis = m_model.axis,
                 .position = { v[0], v[1], v[2] },
                 .speed = v[3],
                 .acceleration = v[4],
                 .deceleration = v[5],
                 .jerk = v[6],
                 .timeout = v[7],
                 .duration = v[8],
                 .latch = v[9],
                 .buffer = m_model.buffer,
                 .continuous = m_model.continuous };
    }
    void Presenter::send(Command c)
    {
        if (!m_model.backend) {
            m_model.report("Motion backend unavailable");
            return;
        }
        if ((c.kind == Kind::MotorRelative || c.kind == Kind::MotorAbsolute || c.kind == Kind::MotorHome) &&
            !c.confirmed && c.buffer == 0 && m_model.state.motor(c.axis).owner) {
            m_confirmation = c;
            m_model.confirmation = true;
            changed = true;
            return;
        }
        const auto outcome = m_model.backend->submit(c);
        m_model.report(outcome.message.data());
        if (outcome.accepted && outcome.job && m_model.page != Page::Preview) {
            m_model.watchedJob = outcome.job;
            m_model.watchedState = ~0U;
        }
    }
    void Presenter::requestDiagnostics()
    {
        if (m_model.diagnosticTab == 2)
            return;
        send(command(m_model.diagnosticTab == 1
                       ? (m_model.diagnosticIO ? Kind::MemoryIOStack : Kind::MemoryUIStack)
                       : (m_model.diagnosticIO ? Kind::MemoryIO : Kind::MemoryUI)));
    }
    void Presenter::act(unsigned a, bool pressed)
    {
        if (!pressed) {
            if (m_model.backend)
                m_model.backend->release();
            return;
        }
        ++m_model.interactions;
        if (a >= Field0 && a <= FieldLast) {
            if (m_model.backend)
                m_model.backend->release();
            m_model.field = a - Field0;
            m_model.keypad = true;
            std::snprintf(
              m_model.number.data(), m_model.number.size(), "%.6g", m_model.values[m_model.field]);
            changed = true;
            return;
        }
        if (a >= Key0 && a <= KeyCancel) {
            auto& text = m_model.number;
            const auto n = std::strlen(text.data());
            if (a <= Key9 && n + 1 < text.size()) {
                text[n] = char('0' + a - Key0);
                text[n + 1] = 0;
            }
            else if (a == KeyDot && !std::strchr(text.data(), '.') && n + 1 < text.size()) {
                text[n] = '.';
                text[n + 1] = 0;
            }
            else if (a == KeySign) {
                if (text[0] == '-')
                    std::memmove(text.data(), text.data() + 1, n);
                else if (n + 1 < text.size()) {
                    std::memmove(text.data() + 1, text.data(), n + 1);
                    text[0] = '-';
                }
            }
            else if (a == KeyBack && n)
                text[n - 1] = 0;
            else if (a == KeyCancel)
                m_model.keypad = false;
            else if (a == KeyApply) {
                char* end{};
                const double v = std::strtod(text.data(), &end);
                const bool signAllowed =
                  m_model.field < 3 || (m_model.page == Page::Motors && m_model.field == 3);
                const bool factor =
                  m_model.field == 10 || ((m_model.page == Page::Robot || m_model.page == Page::Preview) &&
                                          m_model.field >= 4 && m_model.field <= 6);
                if (!n || *end || !std::isfinite(v) || (!signAllowed && v < 0) ||
                    (factor && (v < 0 || v > 1 || (m_model.field == 10 && v == 0))) ||
                    (m_model.field == 11 && v > 100))
                    m_model.report("Invalid value: check units, sign and range");
                else {
                    m_model.values[m_model.field] = v;
                    m_model.keypad = false;
                    if (m_model.field == 11)
                        brightnessChanged = true;
                }
            }
            changed = true;
            return;
        }
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
                if (m_model.settingsTab == 2)
                    requestDiagnostics();
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
                if (m_model.backend)
                    m_model.backend->release();
                m_model.axis = a - Axis1;
                if (m_model.page == Page::Home)
                    navigate(Page::Axes);
                changed = true;
                break;
            case StepSmall:
                m_model.step = .1;
                changed = true;
                break;
            case StepMedium:
                m_model.step = 1;
                changed = true;
                break;
            case StepLarge:
                m_model.step = 10;
                changed = true;
                break;
            case Relative:
                m_model.absolute = false;
                changed = true;
                break;
            case Absolute:
                m_model.absolute = true;
                changed = true;
                break;
            case Timed:
            case Continuous:
                if (m_model.backend)
                    m_model.backend->release();
                m_model.continuous = a == Continuous;
                m_model.jogMode = a == Continuous ? 2 : 1;
                changed = true;
                break;
            case More:
                if (m_model.backend)
                    m_model.backend->release();
                m_model.more = !m_model.more;
                changed = true;
                break;
            case Advanced:
                if (m_model.backend)
                    m_model.backend->release();
                m_model.advanced = !m_model.advanced;
                changed = true;
                break;
            case AxisPosition:
                if (m_model.backend)
                    m_model.backend->release();
                m_model.axisTab = 1;
                changed = true;
                break;
            case StepMode:
                if (m_model.backend)
                    m_model.backend->release();
                m_model.jogMode = 0;
                m_model.continuous = false;
                changed = true;
                break;
            case MotorMoveTab:
            case MotorHomeTab:
            case MotorDefaultsTab:
                if (m_model.backend)
                    m_model.backend->release();
                m_model.motorTab = a - MotorMoveTab;
                changed = true;
                break;
            case SettingsGeneral:
            case SettingsRobot:
            case SettingsDiagnostics:
                m_model.settingsTab = a - SettingsGeneral;
                if (a == SettingsDiagnostics)
                    requestDiagnostics();
                changed = true;
                break;
            case IODigital:
            case IODrivers:
                m_model.ioTab = a - IODigital;
                changed = true;
                break;
            case Driver0:
            case Driver1:
            case Driver2:
                m_model.driverDetail = a - Driver0;
                changed = true;
                break;
            case DiagnosticUI:
            case DiagnosticIO:
                m_model.diagnosticIO = a == DiagnosticIO;
                m_model.diagnosticOffset = 0;
                requestDiagnostics();
                changed = true;
                break;
            case DiagnosticHeap:
            case DiagnosticStacks:
            case DiagnosticGraphics:
                m_model.diagnosticTab = a - DiagnosticHeap;
                if (a == DiagnosticGraphics)
                    m_model.diagnosticIO = false;
                m_model.diagnosticOffset = 0;
                requestDiagnostics();
                changed = true;
                break;
            case DiagnosticRefresh:
                requestDiagnostics();
                changed = true;
                break;
            case DiagnosticPrevious:
                m_model.diagnosticOffset = m_model.diagnosticOffset >= 6 ? m_model.diagnosticOffset - 6 : 0;
                changed = true;
                break;
            case DiagnosticNext:
                if (m_model.diagnosticOffset < 30)
                    m_model.diagnosticOffset += 6;
                changed = true;
                break;
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
                if (m_model.backend)
                    m_model.backend->release();
                send(command(Kind::Stop));
                break;
            case Abort:
                send(command(Kind::MotorAbort));
                break;
            case Reference:
                send(command(m_model.page == Page::Motors ? Kind::MotorHome
                             : m_model.page == Page::Axes ? Kind::AxisHome
                                                          : Kind::RobotHome));
                break;
            case Minus:
            case Plus: {
                auto c = command(Kind::AxisRelative);
                c.position[0] = (a == Minus ? -1 : 1) * m_model.step;
                send(c);
                break;
            }
            case JogMinus:
            case JogPlus: {
                auto c = command(Kind::AxisJog);
                c.speed = (a == JogMinus ? -1 : 1) * (m_model.values[3] > 0
                                                        ? m_model.values[3]
                                                        : .1 * m_model.state.axes[m_model.axis].maxSpeed);
                if (c.continuous && m_model.backend)
                    m_model.report(
                      m_model.backend->hold(c, holdX, holdY, holdWidth, holdHeight).message.data());
                else
                    send(c);
                break;
            }
            case Execute: {
                Kind k =
                  m_model.page == Page::Motors
                    ? (m_model.absolute ? Kind::MotorAbsolute : Kind::MotorRelative)
                  : m_model.page == Page::Axes ? (m_model.absolute ? Kind::AxisAbsolute : Kind::AxisRelative)
                  : m_model.coordinates == 1 ? (m_model.absolute ? Kind::PointAbsolute : Kind::PointRelative)
                                             : (m_model.absolute ? Kind::JointAbsolute : Kind::JointRelative);
                auto c = command(k);
                if (m_model.page == Page::Robot) {
                    c.speed = m_model.values[10];
                    c.acceleration = c.acceleration == 0 ? 1 : c.acceleration;
                    c.deceleration = c.deceleration == 0 ? 1 : c.deceleration;
                    c.jerk = c.jerk == 0 ? 1 : c.jerk;
                }
                send(c);
                break;
            }
            case XMinus:
            case XPlus:
            case YMinus:
            case YPlus:
            case ZMinus:
            case ZPlus: {
                auto c = command(Kind::PointRelative);
                c.position = {};
                c.position[(a - XMinus) / 2] = (a % 2 == XMinus % 2 ? -1 : 1) * m_model.step;
                c.speed = m_model.values[10];
                c.acceleration = c.deceleration = c.jerk = 1;
                c.continuous = false; // Each held increment remains a finite, validated point move.
                if (m_model.backend)
                    m_model.report(
                      m_model.backend->hold(c, holdX, holdY, holdWidth, holdHeight).message.data());
                break;
            }
            case PreviewJoints:
            case PreviewPoint:
            case PreviewLine:
                m_model.coordinates = a == PreviewJoints ? 0 : a == PreviewPoint ? 1 : 2;
                changed = true;
                break;
            case RunPreview: {
                auto c = command(m_model.coordinates == 0   ? Kind::PreviewJoint
                                 : m_model.coordinates == 1 ? Kind::PreviewPoint
                                                            : Kind::PreviewLine);
                c.speed = m_model.values[10];
                send(c);
                break;
            }
            case NextBuffer:
                m_model.buffer = (m_model.buffer + 1) % 6;
                changed = true;
                break;
            case ApplyDefaults:
                send(command(Kind::MotorDefaults));
                break;
            case ActiveSpeed:
                send(command(Kind::MotorSpeed));
                break;
            case ZoomIn:
                cameraIn = true;
                break;
            case ZoomOut:
                cameraOut = true;
                break;
            case ResetView:
                cameraReset = true;
                break;
            case PrevJobs:
                m_model.jobOffset = m_model.jobOffset >= 4 ? m_model.jobOffset - 4 : 0;
                changed = true;
                break;
            case NextJobs:
                if (m_model.jobCount == 4)
                    m_model.jobOffset += 4;
                changed = true;
                break;
            case TogglePreviews:
                if (m_model.page == Page::Preview) {
                    navigate(Page::Jobs);
                    m_model.previews = true;
                    break;
                }
                m_model.previews = !m_model.previews;
                m_model.jobOffset = 0;
                changed = true;
                break;
            case Cancel0:
            case Cancel1:
            case Cancel2:
            case Cancel3: {
                const auto i = a - Cancel0;
                if (i < m_model.jobCount) {
                    auto c = command(m_model.previews ? Kind::CancelPreview : Kind::Cancel);
                    c.job = m_model.jobs[i].id;
                    send(c);
                }
                break;
            }
            case AxisDynamics:
                if (m_model.backend)
                    m_model.backend->release();
                m_model.axisTab = 2;
                changed = true;
                break;
            case AxisJogPage:
                if (m_model.backend)
                    m_model.backend->release();
                m_model.axisTab = 0;
                changed = true;
                break;
            case AxisStop:
                if (m_model.backend)
                    m_model.backend->release();
                send(command(Kind::AxisStop));
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
            case Acknowledge:
                m_model.messageCount = 0;
                m_model.report("Messages acknowledged locally; active faults remain");
                changed = true;
                break;
            case Confirm:
                m_model.confirmation = false;
                m_confirmation.confirmed = true;
                send(m_confirmation);
                changed = true;
                break;
            case Reject:
                m_model.confirmation = false;
                changed = true;
                break;
            default:
                break;
        }
    }
}
