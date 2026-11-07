#pragma once
#include "ui/gui/hmi/View.hpp"
#include <mvp/MVPApplication.hpp>
namespace ui::gui
{
    class Frontend final : public touchgfx::MVPApplication
    {
      public:
        Frontend(scene3d::RenderService& renderer, hmi::Backend* backend);
        ~Frontend() override;
        void changeToStartScreen() override;
        void handleTickEvent() override;
        void handlePendingScreenTransition() override {}
        void setCommands(void (*cb)(void*), void* context)
        {
            m_commands = cb;
            m_context = context;
        }
        void diagnostics(bool enabled) { m_view.diagnostics(enabled); }
        void scene(unsigned triangles) { m_view.benchmark(triangles); }
        void graphics(const char* text)
        {
            std::snprintf(m_model.graphics.data(), m_model.graphics.size(), "%s", text);
        }
        void rate(unsigned fps, bool interactive) { m_view.rate(fps, interactive); }
        bool brightnessChanged() { return m_view.brightnessChanged(); }
        const hmi::Model& model() const { return m_model; }

      private:
        static hmi::Model& reserveModel();
        hmi::Model& m_model;
        hmi::Presenter m_presenter;
        hmi::View m_view;
        void (*m_commands)(void*){};
        void* m_context{};
    };
}
