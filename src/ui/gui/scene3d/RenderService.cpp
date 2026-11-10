#include "RenderService.hpp"
#include "Renderer.hpp"
#include "hal/core/core.hpp"
#include "ui/gui/util/FrameTimer.hpp"
#include <algorithm>
#include <atomic>
namespace ui::gui::scene3d
{
    namespace
    {
        constexpr unsigned CACHE_LINE_BYTES{ 32 }, MAX_FPS{ 30 };
        struct alignas(CACHE_LINE_BYTES) Storage
        {
            std::array<std::array<std::uint16_t, PIXELS>, 2> color;
            std::array<std::uint16_t, PIXELS> depth;
        };
#if defined(CORE_CM7)
        // Linker-reserved render buffers; the atomic claim permits one active renderer.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
        __attribute__((section(".gui_sdram"))) Storage buffers{};
#else
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
        Storage buffers{};
#endif
        // Linker-reserved render buffers; the atomic claim permits one active renderer.
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
        std::atomic_bool claimed{};
    }
    // The view belongs to this service lifetime; linker placement is an implementation detail.
    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    std::span<const std::uint16_t> RenderService::pixels(unsigned slot) const
    {
        return slot < buffers.color.size() ? std::span<const std::uint16_t>{ buffers.color[slot] }
                                           : std::span<const std::uint16_t>{};
    }
    Status RenderService::status() const
    {
        std::scoped_lock lock{ m_mutex };
        return m_status;
    }
    bool RenderService::submit(const Frame& frame)
    {
        if (!frame.scene || !frame.snapshot.valid(*frame.scene) || !frame.maximum_fps ||
            frame.maximum_fps > MAX_FPS) {
            std::scoped_lock lock{ m_mutex };
            ++m_status.rejected;
            return false;
        }
        bool wake{};
        {
            std::scoped_lock lock{ m_mutex };
            if (m_stop || m_status.failed || !m_status.visible) {
                return false;
            }
            if (m_hasPending) {
                ++m_status.replaced;
            }
            wake = !m_hasPending;
            m_pending = frame;
            m_hasPending = true;
        }
        if (wake) {
            m_changed.signal();
        }
        return true;
    }
    void RenderService::visible(bool value)
    {
        {
            std::scoped_lock lock{ m_mutex };
            if (m_status.visible != value) {
                ++m_visibilityGeneration;
            }
            m_status.visible = value;
            if (!value) {
                m_hasPending = false;
                m_front.reset();
                m_ready.reset();
            }
        }
        m_changed.signal();
    }
    std::optional<Image> RenderService::acquire()
    {
        std::optional<Image> result{};
        {
            std::scoped_lock lock{ m_mutex };
            if (m_ready && m_status.visible) {
                m_front = m_ready;
                m_ready.reset();
                const auto& f{ m_frames[*m_front] };
                ++m_status.acquired;
                m_status.acquired_revision = f.revision;
                m_status.source_time_us = f.source_time_us;
                m_status.acquired_triangles = f.triangles;
                result = Image{ .pixels = pixels(*m_front),
                                .slot = *m_front,
                                .revision = f.revision,
                                .source_time_us = f.source_time_us,
                                .annotations = f.annotations };
            }
        }
        if (result) {
            m_changed.signal();
        }
        return result;
    }
    void RenderService::stopAndWait()
    {
        {
            std::scoped_lock lock{ m_mutex };
            m_stop = true;
        }
        m_changed.signal();
        while (status().running) {
            m_finished.waitUntil(std::chrono::steady_clock::time_point::max());
        }
    }
    void RenderService::presented()
    {
        std::scoped_lock lock{ m_mutex };
        if (m_status.visible && m_front && m_status.presented_revision != m_status.acquired_revision) {
            ++m_status.presented;
            m_status.presented_revision = m_status.acquired_revision;
            const auto now{ std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count() };
            m_status.last_latency_us = static_cast<std::uint32_t>(
              std::min<std::uint64_t>(std::uint64_t(now) - m_status.source_time_us, UINT32_MAX));
            m_status.maximum_latency_us = std::max(m_status.maximum_latency_us, m_status.last_latency_us);
        }
    }
    bool RenderService::selectFrameLocked(Work& work, std::chrono::steady_clock::time_point next)
    {
        if (!m_status.visible || !m_hasPending || std::chrono::steady_clock::now() < next) {
            return false;
        }
        for (unsigned i{ 0 }; i < buffers.color.size(); ++i) {
            if (m_front == i || m_ready == i) {
                continue;
            }
            work.frame = m_pending;
            work.slot = i;
            work.generation = m_visibilityGeneration;
            m_hasPending = false;
            return true;
        }
        return false;
    }
    bool RenderService::completeFrame(const Work& work, const Rendered& rendered, const std::stop_token& stop)
    {
        std::scoped_lock lock{ m_mutex };
        m_status.last_render_us = rendered.elapsed;
        m_status.clear_us = rendered.timings.clear_us;
        m_status.geometry_us = rendered.timings.geometry_us;
        m_status.grid_us = rendered.timings.grid_us;
        m_status.node_us = rendered.timings.node_us;
        m_status.maximum_render_us = std::max(m_status.maximum_render_us, rendered.elapsed);
        m_status.total_render_us += rendered.elapsed;
        if (!rendered.ok) {
            m_status.failed = true;
            return false;
        }
        ++m_status.rendered;
        m_status.completed_revision = work.frame.snapshot.revision;
        unsigned triangles{};
        for (unsigned i{ 0 }; i < work.frame.scene->count; ++i) {
            triangles += work.frame.scene->nodes[i].mesh->triangles;
        }
        m_status.completed_triangles = triangles;
        if (m_status.visible && work.generation == m_visibilityGeneration && !m_stop &&
            !stop.stop_requested()) {
            m_frames[work.slot] = { .revision = work.frame.snapshot.revision,
                                    .source_time_us = work.frame.snapshot.source_time_us,
                                    .triangles = triangles,
                                    .annotations = rendered.annotations };
            m_ready = work.slot;
        }
        else {
            ++m_status.discarded;
        }
        return true;
    }
    void RenderService::run(const std::stop_token& stop)
    {
        const std::stop_callback cancellation{ stop, [this] { m_changed.signal(); } };
        {
            std::scoped_lock lock{ m_mutex };
            if (m_stop || stop.stop_requested()) {
                return;
            }
            if (claimed.exchange(true)) {
                m_status.failed = true;
                return;
            }
            m_status.running = true;
        }
        try {
            util::FrameTimer timer{ hal::core::create_runtime_timer() };
            auto next{ std::chrono::steady_clock::now() };
            for (;;) {
                Work work{};
                bool selected{};
                {
                    std::scoped_lock lock{ m_mutex };
                    if (m_stop || stop.stop_requested()) {
                        break;
                    }
                    selected = selectFrameLocked(work, next);
                }
                if (!selected) {
                    if (std::chrono::steady_clock::now() < next) {
                        m_changed.waitUntil(next);
                    }
                    else {
                        m_changed.waitUntil(std::chrono::steady_clock::time_point::max());
                    }
                    continue;
                }
                const auto began{ std::chrono::steady_clock::now() };
                timer.start();
                Rendered rendered{};
                rendered.ok = render(work.frame,
                                     buffers.color[work.slot],
                                     buffers.depth,
                                     &rendered.timings,
                                     &rendered.annotations);
                rendered.elapsed = static_cast<std::uint32_t>(timer.elapsed().count());
                if (!completeFrame(work, rendered, stop)) {
                    break;
                }
                // A bounded rate and at least one tick off also let lower
                // priority logging workers run when rendering is overloaded.
                next = next_render(began, std::chrono::steady_clock::now(), work.frame.maximum_fps);
            }
        } catch (...) {
            std::scoped_lock lock{ m_mutex };
            m_status.failed = true;
        }
        {
            std::scoped_lock lock{ m_mutex };
            m_status.running = false;
            m_hasPending = false;
        }
        claimed.store(false);
        m_finished.signal();
    }
}
