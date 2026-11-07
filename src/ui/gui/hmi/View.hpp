#pragma once
#include "Gestures.hpp"
#include "Presenter.hpp"
#include "Table.hpp"
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
        void rate(unsigned fps, bool interactive) { m_viewport.qualification(fps, interactive); }
        bool brightnessChanged();
        bool input(const hal::device::TouchReport&, bool valid);

      private:
        void build(bool invalidatePage = true);
        void refresh();
        void homePage();
        void movePage();
        void axisPage();
        void robotPage();
        void motorPage();
        void maintenancePage();
        void settingsPage();
        void dialog();
        void card(unsigned index, int x, int y, int width, int height);
        void lamp(int x, int y, std::uint32_t color);
        void value(const char* text, int x, int y, int width, bool large = false);
        Control& button(unsigned id,
                        const char* text,
                        int x,
                        int y,
                        int width,
                        int height = 56,
                        bool enabled = true,
                        bool selected = false,
                        std::uint32_t tint = 0,
                        unsigned radius = 10,
                        bool large = false);

        void control(unsigned id,
                     const char* text,
                     int x,
                     int y,
                     int width,
                     bool enabled = true,
                     bool selected = false);
        void label(const char* text,
                   int x,
                   int y,
                   int width,
                   int height = 32,
                   bool large = false,
                   touchgfx::Alignment alignment = touchgfx::LEFT,
                   Typography size = Typography::Caption);
        void field(unsigned index, const char* name, int x, int y, int width = 200);
        Gestures m_gestures;
        Model& m_model;
        touchgfx::Box m_background;
        std::array<Panel, 9> m_panels;
        std::array<Panel, 7> m_lamps;
        Viewport m_viewport;
        std::array<Control, 8> m_shell;
        Icon m_brand;
        Label m_title, m_health, m_notice;
        std::array<Control, 24> m_controls;
        std::array<Label, 20> m_labels;
        Table m_table;
        Slider m_brightness;
        touchgfx::Image m_axisImage;
        std::array<touchgfx::Box, 8> m_bars;
        unsigned m_controlCount{}, m_labelCount{}, m_lampCount{};
        bool m_livePose{};
        bool m_visible{}, m_diagnostics{}, m_benchmark{};
        std::chrono::steady_clock::time_point m_next{};
    };
}
