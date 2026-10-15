#pragma once
#include "Components.hpp"
#include "Model.hpp"
#include <array>
#include <mvp/MVPApplication.hpp>
#include <mvp/Presenter.hpp>
#include <mvp/View.hpp>
#include <touchgfx/widgets/Image.hpp>
namespace ui::gui
{
    class ShowcaseView;
    class Presenter final : public touchgfx::Presenter
    {
      public:
        Presenter(Model& model, ShowcaseView& view)
          : m_model(model)
          , m_view(view)
        {
        }
        void activate() override;
        void toggle();
        void value(unsigned value);
        void tick();
        void details() { m_model.details = true; }
        const Model& model() const { return m_model; }

      private:
        Model& m_model;
        ShowcaseView& m_view;
    };
    class ShowcaseView final : public touchgfx::View<Presenter>
    {
      public:
        ShowcaseView();
        void handleTickEvent() override;
        void refresh(const Model& model);
        void diagnostics(bool enabled);

      private:
        touchgfx::Box m_background, m_card, m_indicator, m_overlay;
        touchgfx::TextArea m_title, m_subtitle, m_caption;
        touchgfx::TextAreaWithOneWildcard m_value, m_status;
        std::array<touchgfx::Unicode::UnicodeChar, 48> m_valueText{}, m_statusText{};
        Button m_toggle, m_disabled, m_details;
        Slider m_slider;
        std::array<touchgfx::Box, 8> m_bars;
        bool m_diagnostics{};
        std::uint32_t m_previousTick{}, m_interactions{};
    };
    class DetailsPresenter final : public touchgfx::Presenter
    {
      public:
        explicit DetailsPresenter(Model& model)
          : m_model(model)
        {
        }
        void back() { m_model.details = false; }

      private:
        Model& m_model;
    };
    class DetailsView final : public touchgfx::View<DetailsPresenter>
    {
      public:
        DetailsView();
        void setupScreen() override;

      private:
        touchgfx::Box m_background;
        touchgfx::Image m_mark;
        touchgfx::TextArea m_title, m_body;
        Button m_back;
    };
    class Frontend final : public touchgfx::MVPApplication
    {
      public:
        Frontend();
        ~Frontend() override;
        void changeToStartScreen() override;
        void handleTickEvent() override;
        void handlePendingScreenTransition() override { transition(); }
        void setCommands(void (*callback)(void*), void* context)
        {
            m_commands = callback;
            m_context = context;
        }
        void diagnostics(bool enabled);
        const Model& model() const { return m_model; }

      private:
        void (*m_commands)(void*){};
        void* m_context{};
        Model m_model;
        ShowcaseView m_view;
        Presenter m_presenter;
        DetailsView m_detailsView;
        DetailsPresenter m_detailsPresenter;
        bool m_detailsActive{};
        void transition();
    };
}
