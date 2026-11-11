#pragma once
#include "Gestures.hpp"
#include "Layout.hpp"
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
        bool input(const hal::device::TouchReport& /*report*/, bool valid);

      private:
        void build(bool invalidate_page = true);
        void refresh();
        void homePage();
        void homeAxes();
        void homeTCP();
        void homeActions();
        void homeProvenance();
        void movePage();
        void axisPage();
        void axisJogPage();
        void axisPositionPage();
        void axisSettingsPage();
        void robotPage();
        void cartesianPage();
        void cartesianJogPage();
        void motorPage();
        void maintenancePage();
        void settingsPage();
        void networkPage();
        void networkDialog();
        void ioPage();
        void jobsPage();
        void alarmsPage();
        void ioDigitalPage();
        void ioDriversPage();
        void generalSettingsPage();
        void robotSettingsPage();
        void diagnosticsPage();
        void graphicsDiagnosticsPage();
        void memoryDiagnosticTable(const DiagnosticPage& page);
        void dialog();
        void card(unsigned index, layout::Bounds bounds);
        void lamp(int x, int y, std::uint32_t color);
        void lamp(layout::Bounds bounds, std::uint32_t color) { lamp(bounds.x, bounds.y, color); }
        void value(const char* text, int x, int y, int width, bool large = false);
        Control& button(Action id,
                        const char* text,
                        int x,
                        int y,
                        int width,
                        int height = theme::TOUCH_TARGET,
                        bool enabled = true,
                        bool selected = false,
                        std::uint32_t tint = 0,
                        unsigned radius = theme::CONTROL_RADIUS,
                        bool large = false);

        void control(Action id,
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
                   int height = theme::LABEL_HEIGHT,
                   bool large = false,
                   touchgfx::Alignment alignment = touchgfx::LEFT,
                   Typography size = Typography::Caption);
        void field(unsigned index, const char* name, int x, int y, int width = theme::FIELD_WIDTH);
        void value(const char* text, layout::Bounds bounds, bool large = false)
        {
            value(text, bounds.x, bounds.y, bounds.width, large);
        }
        Control& button(Action id,
                        const char* text,
                        layout::Bounds bounds,
                        bool enabled = true,
                        bool selected = false,
                        std::uint32_t tint = 0,
                        unsigned radius = theme::CONTROL_RADIUS,
                        bool large = false)
        {
            return button(id,
                          text,
                          bounds.x,
                          bounds.y,
                          bounds.width,
                          bounds.height,
                          enabled,
                          selected,
                          tint,
                          radius,
                          large);
        }
        void control(Action id,
                     const char* text,
                     layout::Bounds bounds,
                     bool enabled = true,
                     bool selected = false)
        {
            control(id, text, bounds.x, bounds.y, bounds.width, enabled, selected);
        }
        void label(const char* text,
                   layout::Bounds bounds,
                   bool large = false,
                   touchgfx::Alignment alignment = touchgfx::LEFT,
                   Typography size = Typography::Caption)
        {
            label(text, bounds.x, bounds.y, bounds.width, bounds.height, large, alignment, size);
        }
        void field(unsigned index, const char* name, layout::Bounds bounds)
        {
            field(index, name, bounds.x, bounds.y, bounds.width);
        }
        static constexpr unsigned PANEL_CAPACITY{ 9 }, LAMP_CAPACITY{ 7 }, SHELL_CONTROLS{ 8 },
          PAGE_CONTROLS{ 24 }, PAGE_LABELS{ 20 }, DIAGNOSTIC_BARS{ 8 };
        Gestures m_gestures;
        Model& m_model;
        touchgfx::Box m_background;
        std::array<Panel, PANEL_CAPACITY> m_panels;
        std::array<Panel, LAMP_CAPACITY> m_lamps;
        Viewport m_viewport;
        std::array<Control, SHELL_CONTROLS> m_shell;
        Label m_title, m_health, m_notice;
        std::array<Control, PAGE_CONTROLS> m_controls;
        std::array<Label, PAGE_LABELS> m_labels;
        Table m_table;
        Slider m_brightness;
        touchgfx::Image m_axisImage;
        std::array<touchgfx::Box, DIAGNOSTIC_BARS> m_bars;
        unsigned m_controlCount{}, m_labelCount{}, m_lampCount{};
        bool m_livePose{};
        bool m_visible{}, m_diagnostics{}, m_benchmark{};
        std::chrono::steady_clock::time_point m_next{};
    };
}
