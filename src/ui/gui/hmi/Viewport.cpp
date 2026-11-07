#include "Viewport.hpp"
#include "ui/gui/scene3d/Robot.hpp"
#include <algorithm>
#include <stdexcept>
namespace ui::gui::hmi
{
    Viewport::Viewport(scene3d::RenderService& service)
      : m_service{ service }
      , m_pose{ scene3d::robotSnapshot({}) }
    {
        setTouchable(true);
        setVisible(false);
    }
    Viewport::~Viewport()
    {
        if (m_registered) {
            for (auto id : m_bitmaps)
                touchgfx::Bitmap::dynamicBitmapDelete(id);
            touchgfx::Bitmap::removeCache();
        }
    }
    void Viewport::open()
    {
        if (!m_registered) {
            touchgfx::Bitmap::setCache(m_cache.data(), sizeof(m_cache), 2);
            for (unsigned i = 0; i < 2; ++i) {
                m_bitmaps[i] = touchgfx::Bitmap::dynamicBitmapCreateExternal(
                  scene3d::WIDTH, scene3d::HEIGHT, m_service.pixels(i).data(), touchgfx::Bitmap::RGB565);
                if (m_bitmaps[i] == touchgfx::BITMAP_INVALID)
                    throw std::runtime_error("robot bitmap registration failed");
                touchgfx::Bitmap::dynamicBitmapSetSolidRect(m_bitmaps[i],
                                                            { 0, 0, scene3d::WIDTH, scene3d::HEIGHT });
            }
            m_registered = true;
        }
        m_open = true;
        m_detail.invalidate();
        m_service.visible(true);
    }
    void Viewport::close()
    {
        m_open = false;
        m_dragging = false;
        setVisible(false);
        m_service.visible(false);
    }
    void Viewport::pose(const std::array<double, 3>& joints, std::uint64_t sourceUs)
    {
        if (m_joints == joints) {
            m_pose.sourceTimeUs = sourceUs;
            return;
        }
        const auto camera = m_pose.camera;
        m_pose = scene3d::robotSnapshot(joints);
        m_pose.camera = camera;
        m_pose.sourceTimeUs = sourceUs;
        m_joints = joints;
        m_detail.changed(std::chrono::steady_clock::now());
    }
    void Viewport::zoom(float factor)
    {
        const auto distance = std::clamp(m_pose.camera.distance * factor, .4f, 2.f);
        if (distance != m_pose.camera.distance) {
            m_pose.camera.distance = distance;
            m_detail.changed(std::chrono::steady_clock::now());
        }
    }
    void Viewport::reset()
    {
        const auto camera = scene3d::robotSnapshot({}).camera;
        if (m_pose.camera.yaw != camera.yaw || m_pose.camera.pitch != camera.pitch ||
            m_pose.camera.distance != camera.distance) {
            m_pose.camera = camera;
            m_detail.changed(std::chrono::steady_clock::now());
        }
    }
    void Viewport::rate(unsigned fps)
    {
        if (fps == 1 || fps == 2 || fps == 5 || fps == 10)
            m_fps = fps;
    }
    void Viewport::benchmark(unsigned triangles)
    {
        m_benchmark = triangles;
        m_detail.invalidate();
    }
    void Viewport::tick()
    {
        if (!m_open)
            return;
        const auto now = std::chrono::steady_clock::now();
        const auto request = m_detail.request(now);
        if (request || m_benchmark || m_qualification) {
            const auto detail = m_qualification ? (m_testInteractive ? scene3d::RobotDetail::Interactive
                                                                     : scene3d::RobotDetail::Full)
                                                : request.value_or(scene3d::RobotDetail::Full);
            const auto us =
              std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
            auto frame = m_pose;
            frame.revision = ++m_revision;
            // Pose freshness comes from telemetry; never hide transport/render age.
            if (!frame.sourceTimeUs || m_qualification)
                frame.sourceTimeUs = us;
            if (m_qualification)
                frame.camera.yaw += float(us % 12000000) / 12000000 * 6.283185f;
            if (m_benchmark)
                frame = scene3d::benchmarkSnapshot(float(us % 60000000) / 1000000, m_revision, us);
            if (m_service.submit(
                  { m_benchmark ? &scene3d::benchmarkScene(m_benchmark) : &scene3d::robotScene(detail),
                    frame,
                    m_benchmark                                   ? 30U
                    : m_qualification                             ? m_fps
                    : detail == scene3d::RobotDetail::Interactive ? scene3d::DetailPolicy::INTERACTIVE_MAX_FPS
                                                                  : scene3d::LIVE_MAX_FPS }))
                m_detail.submitted(detail);
        }
        if (const auto image = m_service.acquire()) {
            setBitmap(touchgfx::Bitmap(m_bitmaps[image->slot]));
            setVisible(true);
            invalidate();
        }
    }
    void Viewport::handleClickEvent(const touchgfx::ClickEvent& event)
    {
        m_dragging = event.getType() == touchgfx::ClickEvent::PRESSED;
    }
    void Viewport::handleDragEvent(const touchgfx::DragEvent& event)
    {
        if (!m_dragging)
            return;
        const auto yaw = m_pose.camera.yaw - event.getDeltaX() * .008f;
        const auto pitch = std::clamp(m_pose.camera.pitch + event.getDeltaY() * .008f, .05f, 1.45f);
        if (yaw != m_pose.camera.yaw || pitch != m_pose.camera.pitch) {
            m_pose.camera.yaw = yaw;
            m_pose.camera.pitch = pitch;
            m_detail.changed(std::chrono::steady_clock::now());
        }
    }
}
