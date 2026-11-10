#include "Frontend.hpp"
#include <atomic>
#include <memory>
#include <stdexcept>
#if defined(HAL_PLATFORM_LINUX)
#include "hal/linux/DisplayHost.hpp"
// Optional native test observer; never linked or called by the M7 firmware.
extern "C" void hmi_observe_presentation(const ui::gui::hmi::Snapshot*) __attribute__((weak));
extern "C" void hmi_observe_editor(bool, const char*, double) __attribute__((weak));
#endif
namespace ui::gui
{
    namespace
    {
#if defined(HAL_PLATFORM_LINUX)
        constexpr unsigned KEY_BATCH_CAPACITY{ 32 };
#endif
#if defined(CORE_CM7)
        constexpr unsigned STORAGE_ALIGNMENT{ 32 };
#endif
        // Fixed GUI arena; atomic claim enforces one frontend and releases it at teardown.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
        std::atomic_bool claimed{};
#if defined(CORE_CM7)
        // Fixed GUI arena; atomic claim enforces one frontend and releases it at teardown.
        // NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
        alignas(STORAGE_ALIGNMENT)
          __attribute__((section(".dtcm_bss"))) std::array<std::byte, sizeof(hmi::Model)> storage{};
        // NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)
#else
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
        alignas(hmi::Model) std::array<std::byte, sizeof(hmi::Model)> storage{};
#endif
    }
    hmi::Model& Frontend::reserveModel()
    {
        if (claimed.exchange(true)) {
            throw std::logic_error("TouchGFX frontend already active");
        }
        // Placement construction in the correctly aligned, fixed frontend arena.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        return *std::construct_at(reinterpret_cast<hmi::Model*>(storage.data()));
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
#if defined(HAL_PLATFORM_LINUX)
        std::array<char, KEY_BATCH_CAPACITY> keys{};
        const auto count{ hal::linux::display::keyboard(m_model.keypad ? m_model.editor_revision : 0, keys) };
        for (unsigned i{ 0 }; i < count; ++i) {
            m_presenter.key(keys[i]);
        }
#endif
        if (m_commands) {
            m_commands(m_context);
        }
        touchgfx::MVPApplication::handleTickEvent();
#if defined(HAL_PLATFORM_LINUX)
        // Closing or replacing a modal invalidates even keys arriving this tick.
        hal::linux::display::keyboard(m_model.keypad ? m_model.editor_revision : 0, {});
        if (hmi_observe_editor) {
            hmi_observe_editor(m_model.keypad, m_model.number.data(), m_model.values[0]);
        }
        if (hmi_observe_presentation) {
            hmi_observe_presentation(&m_model.state);
        }
#endif
    }
}
