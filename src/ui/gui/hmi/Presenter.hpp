#pragma once
#include "Actions.hpp"
#include "Model.hpp"
#include <mvp/Presenter.hpp>
namespace ui::gui::hmi
{
    class Presenter final : public touchgfx::Presenter
    {
      public:
        Presenter(const Presenter&) = delete;
        Presenter& operator=(const Presenter&) = delete;
        Presenter(Presenter&&) = delete;
        Presenter& operator=(Presenter&&) = delete;
        ~Presenter() override = default;

        explicit Presenter(Model& model)
          : m_model{ model }
        {
        }
        void tick();
        void navigate(Page page);
        void act(Action action, bool pressed);
        void key(char key);
        Command command(Kind kind) const;
        void send(Command command);
        void requestDiagnostics();
        struct Updates
        {
            bool changed{};
            bool camera_reset{}, camera_in{}, camera_out{}, brightness_changed{};
            int hold_x{}, hold_y{}, hold_width{}, hold_height{};
        };
        Updates& pending() noexcept { return m_pending; }

      private:
        void editNumber(Action action);
        bool navigation(Action action);
        void releaseHeld() const;
        bool movementModes(Action action);
        bool diagnosticActions(Action action);
        bool motionActions(Action action);
        bool historyActions(Action action);
        bool networkActions(Action action);
        void editText(char key);
        void jogAxis(Action action);
        void executeMove(Action action);
        void jogCartesian(Action action);
        Updates m_pending{};
        Model& m_model;
        Command m_confirmation{};
    };
}
