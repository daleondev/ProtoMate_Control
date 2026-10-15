#include "Frontend.hpp"
#include "Theme.hpp"
#include <images/BitmapDatabase.hpp>
#include <texts/TextKeysAndLanguages.hpp>
#include <chrono>
#include <stdexcept>
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
        if (m_model.details == m_detailsActive && m_sceneActive == m_sceneRequested) {
            return;
        }
        currentPresenter->deactivate();
        m_detailsActive = m_model.details;
        m_sceneActive = m_sceneRequested;
        currentPresenter =
          m_detailsActive ? static_cast<touchgfx::Presenter*>(&m_detailsPresenter) : &m_presenter;
        currentPresenter->activate();
        switchScreen(m_sceneActive ? static_cast<touchgfx::Screen*>(&m_sceneView) :
                     m_detailsActive ? static_cast<touchgfx::Screen*>(&m_detailsView) : &m_view);
    }
    void Frontend::diagnostics(bool enabled)
    {
        m_sceneRequested = false;
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
    Frontend::Frontend(scene3d::RenderService& renderer)
      : m_presenter(m_model, m_view)
      , m_detailsPresenter(m_model)
      , m_sceneView(renderer)
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
    void Frontend::scene(unsigned triangles)
    {
        m_sceneView.triangles = triangles;
        m_model.details = false;
        m_sceneRequested = true;
    }
    Frontend::SceneView::SceneView(scene3d::RenderService& renderer) : m_renderer(renderer)
    {
        m_background.setPosition(0,0,1024,600);
        m_background.setColor(theme::BACKGROUND);
        add(m_background);
        // Odd coordinates deliberately exercise cached source / DMA alignment.
        m_image.setXY(49,137);
        add(m_image);
        m_label.setPosition(48,48,928,64);
        m_label.setTypedText(touchgfx::TypedText(T_BODYVALUE));
        m_label.setWildcard(m_labelText.data());
        m_label.setColor(theme::TEXT);
        add(m_label);
    }
    void Frontend::SceneView::setupScreen()
    {
        if (!m_registered) {
            touchgfx::Bitmap::setCache(m_cache.data(), sizeof(m_cache), 2);
            for (unsigned i=0;i<2;++i) {
                m_bitmaps[i]=touchgfx::Bitmap::dynamicBitmapCreateExternal(
                  scene3d::WIDTH,scene3d::HEIGHT,m_renderer.pixels(i).data(),touchgfx::Bitmap::RGB565);
                if (m_bitmaps[i]==touchgfx::BITMAP_INVALID) { throw std::runtime_error("3D bitmap registration failed"); }
                touchgfx::Bitmap::dynamicBitmapSetSolidRect(m_bitmaps[i], {0,0,scene3d::WIDTH,scene3d::HEIGHT});
            }
            m_registered=true;
        }
        m_image.setVisible(false);
        m_renderer.visible(true);
    }
    void Frontend::SceneView::tearDownScreen() { m_renderer.visible(false); }
    Frontend::SceneView::~SceneView()
    {
        if (m_registered) {
            for (auto id:m_bitmaps) { touchgfx::Bitmap::dynamicBitmapDelete(id); }
            touchgfx::Bitmap::removeCache();
        }
    }
    void Frontend::SceneView::handleTickEvent()
    {
        const auto time=std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count();
        m_renderer.submit({&scene3d::benchmarkScene(triangles),
          scene3d::benchmarkSnapshot(float(time%60000000)/1000000, ++m_revision, time)});
        if (const auto image=m_renderer.acquire()) {
            m_image.setBitmap(touchgfx::Bitmap(m_bitmaps[image->slot]));
            m_image.setVisible(true);
            m_image.invalidate();
        }
        if (m_revision%60==1) {
            touchgfx::Unicode::snprintf(m_labelText.data(),m_labelText.size(),
              "Shaded 3D benchmark | %u triangles | 640 x 400",triangles);
            m_label.setTypedText(m_label.getTypedText());
            m_label.invalidate();
        }
    }
}
