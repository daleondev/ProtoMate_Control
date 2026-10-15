#include "Frontend.hpp"
#include "Theme.hpp"
#include <images/BitmapDatabase.hpp>
#include <texts/TextKeysAndLanguages.hpp>
namespace ui::gui
{
    void Presenter::activate() { m_view.refresh(m_model); }
    void Presenter::toggle()
    {
        m_model.animating = !m_model.animating;
        ++m_model.interactions;
        m_view.refresh(m_model);
    }
    void Presenter::value(unsigned value)
    {
        m_model.value = value;
        ++m_model.interactions;
        m_view.refresh(m_model);
    }
    void Presenter::tick()
    {
        m_model.tick();
        m_view.refresh(m_model);
    }
    ShowcaseView::ShowcaseView()
    {
        m_background.setPosition(0, 0, 1024, 600);
        m_background.setColor(theme::BACKGROUND);
        add(m_background);
        m_title.setPosition(48, 40, 900, 56);
        m_title.setTypedText(touchgfx::TypedText(T_TITLE));
        m_title.setColor(theme::TEXT);
        add(m_title);
        m_subtitle.setPosition(48, 104, 900, 40);
        m_subtitle.setTypedText(touchgfx::TypedText(T_SUBTITLE));
        m_subtitle.setColor(theme::MUTED);
        add(m_subtitle);
        m_card.setPosition(48, 176, 928, 312);
        m_card.setColor(theme::SURFACE);
        add(m_card);
        m_toggle.setXY(80, 208);
        m_toggle.configure(
          "Pause animation", [](void* p) { static_cast<ShowcaseView*>(p)->presenter->toggle(); }, this);
        add(m_toggle);
        m_disabled.setXY(80, 288);
        m_disabled.configure("Coming later", nullptr, nullptr);
        m_disabled.enabled(false);
        add(m_disabled);
        m_slider.setXY(496, 252);
        m_slider.configure(
          [](void* p, unsigned value) { static_cast<ShowcaseView*>(p)->presenter->value(value); }, this);
        add(m_slider);
        m_value.setPosition(504, 208, 400, 36);
        m_value.setTypedText(touchgfx::TypedText(T_BODYVALUE));
        m_value.setWildcard(m_valueText.data());
        m_value.setColor(theme::TEXT);
        add(m_value);
        m_status.setPosition(80, 400, 760, 36);
        m_status.setTypedText(touchgfx::TypedText(T_CAPTIONVALUE));
        m_status.setWildcard(m_statusText.data());
        m_status.setColor(theme::MUTED);
        add(m_status);
        m_indicator.setPosition(504, 344, 33, 33);
        m_indicator.setColor(theme::ACCENT);
        add(m_indicator);
        m_overlay.setPosition(680, 344, 97, 33);
        m_overlay.setColor(theme::TEXT);
        m_overlay.setAlpha(80);
        add(m_overlay);
        m_caption.setPosition(48, 528, 560, 36);
        m_caption.setTypedText(touchgfx::TypedText(T_CAPTION));
        m_caption.setColor(theme::MUTED);
        add(m_caption);
        m_details.setXY(688, 512);
        m_details.configure(
          "Framework details", [](void* p) { static_cast<ShowcaseView*>(p)->presenter->details(); }, this);
        add(m_details);
        constexpr std::array<std::uint32_t, 8> colors{ 0xffffff, 0xffff00, 0x00ffff, 0x00ff00,
                                                       0xff00ff, 0xff0000, 0x0000ff, 0 };
        for (unsigned i = 0; i < m_bars.size(); ++i) {
            m_bars[i].setPosition(i * 128, 0, 128, 600);
            m_bars[i].setColor(colors[i]);
            m_bars[i].setVisible(false);
            add(m_bars[i]);
        }
    }
    void ShowcaseView::handleTickEvent()
    {
        if (!m_diagnostics && presenter) {
            presenter->tick();
        }
    }
    void ShowcaseView::refresh(const Model& model)
    {
        if (m_previousTick != model.ticks) {
            m_indicator.invalidate();
            const auto phase = model.ticks % theme::ANIMATION_FRAMES;
            m_indicator.setX(504 + int((phase < 60 ? phase : 120 - phase) * 6));
            m_indicator.invalidate();
            m_previousTick = model.ticks;
        }
        // Update text/widgets only on interaction, keeping animation dirty regions small.
        const auto expected = model.value;
        if (m_valueText[0] == 0 || model.interactions != m_interactions) {
            m_interactions = model.interactions;
            touchgfx::Unicode::snprintf(m_valueText.data(), m_valueText.size(), "Value  %u / 100", expected);
            touchgfx::Unicode::strncpy(m_statusText.data(),
                                       model.animating ? "Local showcase  |  animation running"
                                                       : "Local showcase  |  animation paused",
                                       m_statusText.size() - 1);
            // TouchGFX caches text bounds; recompute after changing wildcards.
            m_value.setTypedText(m_value.getTypedText());
            m_status.setTypedText(m_status.getTypedText());
            m_slider.value(expected);
            m_value.invalidate();
            m_status.invalidate();
            m_toggle.configure(model.animating ? "Pause animation" : "Resume animation", [](void* p) {
                static_cast<ShowcaseView*>(p)->presenter->toggle();
            }, this);
        }
    }
    void ShowcaseView::diagnostics(bool enabled)
    {
        if (m_diagnostics == enabled) {
            return;
        }
        m_diagnostics = enabled;
        for (auto& bar : m_bars) {
            bar.setVisible(enabled);
        }
        invalidate();
    }
    DetailsView::DetailsView()
    {
        m_background.setPosition(0, 0, 1024, 600);
        m_background.setColor(theme::BACKGROUND);
        add(m_background);
        m_mark.setXY(48, 48);
        add(m_mark);
        m_title.setPosition(120, 48, 856, 56);
        m_title.setTypedText(touchgfx::TypedText(T_DETAILSTITLE));
        m_title.setColor(theme::TEXT);
        add(m_title);
        m_body.setPosition(48, 176, 928, 240);
        m_body.setTypedText(touchgfx::TypedText(T_DETAILSBODY));
        m_body.setColor(theme::MUTED);
        add(m_body);
        m_back.setXY(48, 512);
        m_back.configure(
          "Back to showcase", [](void* p) { static_cast<DetailsView*>(p)->presenter->back(); }, this);
        add(m_back);
    }
    void DetailsView::setupScreen() { m_mark.setBitmap(touchgfx::Bitmap(BITMAP_STUDIO_ID)); }
    void Frontend::transition()
    {
        if (m_model.details == m_detailsActive) {
            return;
        }
        currentPresenter->deactivate();
        m_detailsActive = m_model.details;
        currentPresenter =
          m_detailsActive ? static_cast<touchgfx::Presenter*>(&m_detailsPresenter) : &m_presenter;
        currentPresenter->activate();
        switchScreen(m_detailsActive ? static_cast<touchgfx::Screen*>(&m_detailsView) : &m_view);
    }
    void Frontend::diagnostics(bool enabled)
    {
        m_model.details = false;
        m_view.diagnostics(enabled);
    }
    void Frontend::handleTickEvent()
    {
        if (m_commands) {
            m_commands(m_context);
        }
        touchgfx::MVPApplication::handleTickEvent();
    }
    Frontend::Frontend()
      : m_presenter(m_model, m_view)
      , m_detailsPresenter(m_model)
    {
        m_view.bind(m_presenter);
        m_detailsView.bind(m_detailsPresenter);
        currentPresenter = &m_presenter;
    }
    Frontend::~Frontend()
    {
        currentScreen = nullptr;
        instance = nullptr;
    }
    void Frontend::changeToStartScreen()
    {
        m_presenter.activate();
        switchScreen(&m_view);
    }
}
