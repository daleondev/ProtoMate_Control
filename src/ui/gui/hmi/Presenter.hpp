#pragma once
#include "Model.hpp"
#include <mvp/Presenter.hpp>
namespace ui::gui::hmi
{
    enum Action : unsigned
    {
        Home,
        Move,
        Programs,
        IO,
        Settings,
        Jobs,
        Alarms,
        GlobalStop,
        Axes,
        Robot,
        Cartesian,
        Preview,
        Motors,
        Presets,
        Axis1,
        Axis2,
        Axis3,
        StepSmall,
        StepMedium,
        StepLarge,
        Minus,
        Plus,
        Reference,
        Execute,
        Relative,
        Absolute,
        Timed,
        Continuous,
        JogMinus,
        JogPlus,
        Initialize,
        Enable,
        Disable,
        Abort,
        Feedback,
        ZoomIn,
        ZoomOut,
        ResetView,
        XMinus,
        XPlus,
        YMinus,
        YPlus,
        ZMinus,
        ZPlus,
        PreviewJoints,
        PreviewPoint,
        PreviewLine,
        NextBuffer,
        ApplyDefaults,
        ActiveSpeed,
        PrevJobs,
        NextJobs,
        TogglePreviews,
        Cancel0,
        Cancel1,
        Cancel2,
        Cancel3,
        MemoryUI,
        MemoryIO,
        Acknowledge,
        AxisDynamics,
        AxisStop,
        AxisJogPage,
        MemoryUIStack,
        MemoryIOStack,
        RunPreview,
        Field0 = 100,
        FieldLast = 111,
        Key0 = 200,
        Key9 = 209,
        KeyDot,
        KeySign,
        KeyBack,
        KeyApply,
        KeyCancel,
        Confirm,
        Reject,
        More = 300,
        AxisPosition,
        StepMode,
        Advanced,
        MotorMoveTab,
        MotorHomeTab,
        MotorDefaultsTab,
        SettingsGeneral,
        SettingsRobot,
        SettingsDiagnostics,
        IODigital,
        IODrivers,
        IOAnalog,
        Driver0,
        Driver1,
        Driver2,
        DiagnosticUI,
        DiagnosticIO,
        DiagnosticHeap,
        DiagnosticStacks,
        DiagnosticGraphics,
        DiagnosticRefresh,
        DiagnosticPrevious,
        DiagnosticNext
    };
    class Presenter final : public touchgfx::Presenter
    {
      public:
        explicit Presenter(Model& model)
          : m_model{ model }
        {
        }
        void tick();
        void navigate(Page page);
        void act(unsigned action, bool pressed);
        Command command(Kind kind) const;
        void send(Command command);
        void requestDiagnostics();
        bool changed{};
        bool cameraReset{}, cameraIn{}, cameraOut{}, brightnessChanged{};
        int holdX{}, holdY{}, holdWidth{}, holdHeight{};

      private:
        Model& m_model;
        Command m_confirmation{};
    };
}
