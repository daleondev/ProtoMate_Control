#pragma once
#include "Renderer.hpp"
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
        std::uint32_t last_render_us{}, maximum_render_us{}, clear_us{}, geometry_us{}, grid_us{};
        std::array<std::uint32_t, 4> node_us{};
        std::uint64_t total_render_us{}, completed_revision{}, acquired_revision{}, source_time_us{};
        std::uint64_t presented_revision{};
        std::uint32_t last_latency_us{}, maximum_latency_us{};
        unsigned completed_triangles{}, acquired_triangles{};
    };
    struct Image
    {
        std::span<const std::uint16_t> pixels;
        unsigned slot{};
        std::uint64_t revision{}, source_time_us{};
        std::array<ProjectedAnchor, 3> annotations{};
    };
    class RenderService final
    {
      public:
        // Exactly one instance may run at a time: the application owns the
        // fixed graphics-memory reservation and a single active viewport.
        void run(const std::stop_token& stop);
        bool submit(const Frame& frame);
        void visible(bool value);       // GUI thread, after its prior draw completed.
        std::optional<Image> acquire(); // GUI thread; releases the prior image.
        void presented();               // GUI thread, after successful LTDC/SDL presentation.
        std::span<const std::uint16_t> pixels(unsigned slot) const;
        Status status() const;
        void stopAndWait(); // GUI shutdown, outside all display/renderer locks.

      private:
        struct Work
        {
            Frame frame{};
            unsigned slot{};
            std::uint64_t generation{};
        };
        struct Rendered
        {
            RenderTimings timings{};
            std::array<ProjectedAnchor, 3> annotations{};
            std::uint32_t elapsed{};
            bool ok{};
        };
        bool selectFrameLocked(Work& /*work*/, std::chrono::steady_clock::time_point next);
        bool completeFrame(const Work& /*work*/,
                           const Rendered& /*rendered*/,
                           const std::stop_token& /*stop*/);
        mutable std::mutex m_mutex;
        runtime::Notification m_changed, m_finished;
        Status m_status;
        Frame m_pending{};
        bool m_hasPending{}, m_stop{};
        std::uint64_t m_visibilityGeneration{};
        std::optional<unsigned> m_front, m_ready;
        struct Completed
        {
            std::uint64_t revision{}, source_time_us{};
            unsigned triangles{};
            std::array<ProjectedAnchor, 3> annotations{};
        };
        std::array<Completed, 2> m_frames{};
    };
}
