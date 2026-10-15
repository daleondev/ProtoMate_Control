#pragma once
#include "Scene.hpp"
#include "runtime/synchronization/Notification.hpp"
#include <chrono>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
namespace ui::gui::scene3d
{
    struct Status
    {
        bool running{}, visible{}, failed{};
        std::uint32_t rendered{}, acquired{}, presented{}, replaced{}, rejected{}, discarded{};
        std::uint32_t lastRenderUs{}, maximumRenderUs{}, clearUs{}, geometryUs{};
        std::uint64_t totalRenderUs{}, completedRevision{}, acquiredRevision{}, sourceTimeUs{};
        std::uint64_t presentedRevision{};
        std::uint32_t lastLatencyUs{}, maximumLatencyUs{};
    };
    struct Image
    {
        std::span<const std::uint16_t> pixels;
        unsigned slot{};
        std::uint64_t revision{}, sourceTimeUs{};
    };
    class RenderService final
    {
      public:
        // Exactly one instance may run at a time: the application owns the
        // fixed graphics-memory reservation and a single active viewport.
        void run(std::stop_token stop);
        bool submit(const Frame& frame);
        void visible(bool value);       // GUI thread, after its prior draw completed.
        std::optional<Image> acquire(); // GUI thread; releases the prior image.
        void presented();               // GUI thread, after successful LTDC/SDL presentation.
        std::span<const std::uint16_t> pixels(unsigned slot) const;
        Status status() const;
        void stopAndWait(); // GUI shutdown, outside all display/renderer locks.

      private:
        mutable std::mutex m_mutex;
        runtime::Notification m_changed, m_finished;
        Status m_status;
        Frame m_pending{};
        bool m_hasPending{}, m_stop{};
        std::uint64_t m_visibilityGeneration{};
        std::optional<unsigned> m_front, m_ready;
        struct Completed
        {
            std::uint64_t revision{}, sourceTimeUs{};
        };
        std::array<Completed, 2> m_frames{};
    };
}
