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
        struct alignas(32) Storage
        {
            std::uint16_t color[2][PIXELS];
            std::uint16_t depth[PIXELS];
        };
#if defined(CORE_CM7)
        __attribute__((section(".gui_sdram"))) Storage buffers;
#else
        Storage buffers;
#endif
        std::atomic_bool claimed{};
    }
    std::span<const std::uint16_t> RenderService::pixels(unsigned slot) const
    {
        return slot < 2 ? std::span<const std::uint16_t>{ buffers.color[slot] }
                        : std::span<const std::uint16_t>{};
    }
    Status RenderService::status() const
    {
        std::scoped_lock lock(m_mutex);
        return m_status;
    }
    bool RenderService::submit(const Frame& frame)
    {
        if (!frame.scene || !frame.snapshot.valid(*frame.scene)) {
            std::scoped_lock lock(m_mutex);
            ++m_status.rejected;
            return false;
        }
        {
            std::scoped_lock lock(m_mutex);
            if (m_stop || m_status.failed || !m_status.visible) {
                return false;
            }
            if (m_hasPending) {
                ++m_status.replaced;
            }
            m_pending = frame;
            m_hasPending = true;
        }
        m_changed.signal();
        return true;
    }
    void RenderService::visible(bool value)
    {
        {
            std::scoped_lock lock(m_mutex);
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
        std::optional<Image> result;
        {
            std::scoped_lock lock(m_mutex);
            if (m_ready && m_status.visible) {
                m_front = m_ready;
                m_ready.reset();
                const auto& f = m_frames[*m_front];
                ++m_status.acquired;
                m_status.acquiredRevision = f.revision;
                m_status.sourceTimeUs = f.sourceTimeUs;
                result = Image{ pixels(*m_front), *m_front, f.revision, f.sourceTimeUs };
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
            std::scoped_lock lock(m_mutex);
            m_stop = true;
        }
        m_changed.signal();
        while (status().running) {
            m_finished.waitUntil(std::chrono::steady_clock::time_point::max());
        }
    }
    void RenderService::presented()
    {
        std::scoped_lock lock(m_mutex);
        if (m_status.visible && m_front && m_status.presentedRevision != m_status.acquiredRevision) {
            ++m_status.presented;
            m_status.presentedRevision = m_status.acquiredRevision;
            const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
            m_status.lastLatencyUs = static_cast<std::uint32_t>(
              std::min<std::uint64_t>(std::uint64_t(now) - m_status.sourceTimeUs, UINT32_MAX));
            m_status.maximumLatencyUs = std::max(m_status.maximumLatencyUs, m_status.lastLatencyUs);
        }
    }
    void RenderService::run(std::stop_token stop)
    {
        const std::stop_callback cancellation(stop, [this] { m_changed.signal(); });
        {
            std::scoped_lock lock(m_mutex);
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
            auto next = std::chrono::steady_clock::now();
            for (;;) {
                Frame frame;
                std::optional<unsigned> slot;
                std::uint64_t generation{};
                {
                    std::scoped_lock lock(m_mutex);
                    if (m_stop || stop.stop_requested()) {
                        break;
                    }
                    if (m_status.visible && m_hasPending && std::chrono::steady_clock::now() >= next) {
                        for (unsigned i = 0; i < 2; ++i) {
                            if (m_front != i && m_ready != i) {
                                slot = i;
                                break;
                            }
                        }
                        if (slot) {
                            frame = m_pending;
                            m_hasPending = false;
                            generation = m_visibilityGeneration;
                        }
                    }
                }
                if (!slot) {
                    if (std::chrono::steady_clock::now() < next) {
                        m_changed.waitUntil(next);
                    }
                    else {
                        m_changed.waitUntil(std::chrono::steady_clock::time_point::max());
                    }
                    continue;
                }
                const auto began = std::chrono::steady_clock::now();
                timer.start();
                RenderTimings timings;
                const auto ok = render(frame, buffers.color[*slot], buffers.depth, &timings);
                const auto elapsed = static_cast<std::uint32_t>(timer.elapsed().count());
                {
                    std::scoped_lock lock(m_mutex);
                    m_status.lastRenderUs = elapsed;
                    m_status.clearUs = timings.clearUs;
                    m_status.geometryUs = timings.geometryUs;
                    m_status.maximumRenderUs = std::max(m_status.maximumRenderUs, elapsed);
                    m_status.totalRenderUs += elapsed;
                    if (!ok) {
                        m_status.failed = true;
                        break;
                    }
                    ++m_status.rendered;
                    m_status.completedRevision = frame.snapshot.revision;
                    if (m_status.visible && generation == m_visibilityGeneration && !m_stop &&
                        !stop.stop_requested()) {
                        m_frames[*slot] = { frame.snapshot.revision, frame.snapshot.sourceTimeUs };
                        m_ready = slot;
                    }
                    else {
                        ++m_status.discarded;
                    }
                }
                // A bounded rate and at least one tick off also let lower
                // priority logging workers run when rendering is overloaded.
                next = std::max(began + std::chrono::microseconds{ 33333 },
                                std::chrono::steady_clock::now() + std::chrono::milliseconds{ 1 });
            }
        } catch (...) {
            std::scoped_lock lock(m_mutex);
            m_status.failed = true;
        }
        {
            std::scoped_lock lock(m_mutex);
            m_status.running = false;
            m_hasPending = false;
        }
        claimed.store(false);
        m_finished.signal();
    }
}
