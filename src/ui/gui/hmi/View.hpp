#pragma once
#include "Presenter.hpp"
#include "Viewport.hpp"
#include "Widgets.hpp"
#include <mvp/View.hpp>
#include <touchgfx/widgets/Box.hpp>
namespace ui::gui::hmi
{
    class View final : public touchgfx::View<Presenter>
    {
      public:
        View(Model& model, scene3d::RenderService& renderer);
        void setupScreen() override;
        void tearDownScreen() override;
        void handleTickEvent() override;
        void diagnostics(bool enabled);
        void benchmark(unsigned triangles);
        void rate(unsigned fps) { m_viewport.qualification(fps); }
        bool brightnessChanged();

      private:
        void build(bool invalidatePage = true);
        void refresh();
        void control(unsigned id,
                     const char* text,
                     int x,
                     int y,
                     int width,
                     bool enabled = true,
                     bool selected = false);
        void label(const char* text, int x, int y, int width, int height = 32, bool large = false);
        void field(unsigned index, const char* name, int x, int y, int width = 200);
        Model& m_model;
        touchgfx::Box m_background;
        std::array<Panel, 4> m_panels;
        Viewport m_viewport;
        std::array<Control, 8> m_shell;
        Label m_title, m_health, m_notice;
        std::array<Control, 24> m_controls;
        std::array<Label, 14> m_labels;
        std::array<touchgfx::Box, 8> m_bars;
        unsigned m_controlCount{}, m_labelCount{};
        bool m_livePose{};
        bool m_visible{}, m_diagnostics{}, m_benchmark{};
        std::chrono::steady_clock::time_point m_next{};
    };
}
