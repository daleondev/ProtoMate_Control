#include "Frontend.hpp"
#include <atomic>
#include <memory>
#include <stdexcept>
#if defined(HAL_PLATFORM_LINUX)
// Optional native test observer; never linked or called by the M7 firmware.
extern "C" void hmi_observe_presentation(const ui::gui::hmi::Snapshot*) __attribute__((weak));
#endif
namespace ui::gui
{
    namespace
    {
        std::atomic_bool claimed{};
#if defined(CORE_CM7)
        alignas(32) __attribute__((section(".dtcm_bss"))) std::byte storage[sizeof(hmi::Model)];
#else
        alignas(hmi::Model) std::byte storage[sizeof(hmi::Model)];
#endif
    }
    hmi::Model& Frontend::reserveModel()
    {
        if (claimed.exchange(true))
            throw std::logic_error("TouchGFX frontend already active");
        return *std::construct_at(reinterpret_cast<hmi::Model*>(storage));
    }

    Frontend::Frontend(scene3d::RenderService& renderer, hmi::Backend* backend)
      : m_model{ reserveModel() }
      , m_presenter{ m_model }
      , m_view{ m_model, renderer }
    {
        m_model.backend = backend;
        m_view.bind(m_presenter);
        currentPresenter = &m_presenter;
    }
    Frontend::~Frontend()
    {
        currentScreen = nullptr;
        instance = nullptr;
        std::destroy_at(&m_model);
        claimed = false;
    }
    void Frontend::changeToStartScreen()
    {
        m_presenter.activate();
        switchScreen(&m_view);
    }
    void Frontend::handleTickEvent()
    {
        if (m_commands)
            m_commands(m_context);
        touchgfx::MVPApplication::handleTickEvent();
#if defined(HAL_PLATFORM_LINUX)
        if (hmi_observe_presentation)
            hmi_observe_presentation(&m_model.state);
#endif
    }
}
