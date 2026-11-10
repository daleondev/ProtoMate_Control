#include "Viewport.hpp"
#include "Annotations.hpp"
#include "Text.hpp"
#include "Widgets.hpp"
#include "ui/gui/frontend/Theme.hpp"
#include "ui/gui/scene3d/Robot.hpp"
#include <algorithm>
#include <array>
#include <numbers>
#include <stdexcept>
#include <touchgfx/hal/HAL.hpp>
namespace ui::gui::hmi
{
    namespace
    {
        constexpr std::array QUALIFICATION_RATES{ 1U, 2U, 5U, 10U };
        constexpr unsigned BENCHMARK_FPS{ 30 }, OPAQUE_ALPHA{ 255 };
        constexpr float CAMERA_NEAR{ .4f }, CAMERA_FAR{ 2.F }, ORBIT_SENSITIVITY{ .008f }, PITCH_MIN{ .05f },
          PITCH_MAX{ 1.45F };
        constexpr std::int64_t ORBIT_PERIOD_US{ 12'000'000 }, BENCHMARK_PERIOD_US{ 60'000'000 },
          MICROSECONDS_PER_SECOND{ 1'000'000 };
        // Preserve the established qualification camera path exactly.
        constexpr float ORBIT_TURN{ 6.283185F };
        constexpr double MILLIMETRES_PER_METRE{ 1000 }, DEGREES_PER_RADIAN{ 180. / std::numbers::pi };
        constexpr std::uint32_t BADGE_COLOR{ 0x192a36 };
        constexpr int BADGE_RADIUS{ 8 }, ANCHOR_MARKER_SIZE{ 5 }, BADGE_TEXT_PADDING{ 6 },
          BADGE_TEXT_HEIGHT{ 48 };
        constexpr unsigned BADGE_TEXT_BYTES{ 48 };
        double joint_units(unsigned axis) { return axis == 2 ? MILLIMETRES_PER_METRE : DEGREES_PER_RADIAN; }
    }
    Viewport::Viewport(scene3d::RenderService& service)
      : m_service{ service }
      , m_pose{ scene3d::robot_snapshot({}) }
    {
        m_bitmaps.fill(touchgfx::BITMAP_INVALID);
        setTouchable(true);
        setVisible(false);
    }
    Viewport::~Viewport() { unregisterBitmaps(); }
    void Viewport::unregisterBitmaps() noexcept
    {
        if (!m_registered) {
            return;
        }
        for (auto& id : m_bitmaps) {
            if (id != touchgfx::BITMAP_INVALID) {
                touchgfx::Bitmap::dynamicBitmapDelete(id);
            }
            id = touchgfx::BITMAP_INVALID;
        }
        touchgfx::Bitmap::removeCache();
        m_registered = false;
    }
    void Viewport::open()
    {
        if (!m_registered) {
            touchgfx::Bitmap::setCache(m_cache.data(), sizeof(m_cache), m_bitmaps.size());
            m_registered = true;
            try {
                for (unsigned i{ 0 }; i < m_bitmaps.size(); ++i) {
                    m_bitmaps[i] = touchgfx::Bitmap::dynamicBitmapCreateExternal(
                      scene3d::WIDTH, scene3d::HEIGHT, m_service.pixels(i).data(), touchgfx::Bitmap::RGB565);
                    if (m_bitmaps[i] == touchgfx::BITMAP_INVALID) {
                        throw std::runtime_error("robot bitmap registration failed");
                    }
                    touchgfx::Bitmap::dynamicBitmapSetSolidRect(m_bitmaps[i],
                                                                { 0, 0, scene3d::WIDTH, scene3d::HEIGHT });
                }
            } catch (...) {
                unregisterBitmaps();
                throw;
            }
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
    void Viewport::pose(const std::array<double, 3>& joints, std::uint64_t source_us)
    {
        if (m_joints == joints) {
            m_pose.source_time_us = source_us;
            return;
        }
        const auto camera{ m_pose.camera };
        m_pose = scene3d::robot_snapshot(joints);
        m_pose.camera = camera;
        m_pose.source_time_us = source_us;
        m_joints = joints;
        m_detail.changed(std::chrono::steady_clock::now());
    }
    void Viewport::zoom(float factor)
    {
        const auto distance{ std::clamp(m_pose.camera.distance * factor, CAMERA_NEAR, CAMERA_FAR) };
        if (distance != m_pose.camera.distance) {
            m_pose.camera.distance = distance;
            m_detail.changed(std::chrono::steady_clock::now());
        }
    }
    void Viewport::reset()
    {
        const auto camera{ scene3d::robot_snapshot({}).camera };
        if (m_pose.camera.yaw != camera.yaw || m_pose.camera.pitch != camera.pitch ||
            m_pose.camera.distance != camera.distance) {
            m_pose.camera = camera;
            m_detail.changed(std::chrono::steady_clock::now());
        }
    }
    void Viewport::rate(unsigned fps)
    {
        if (std::ranges::contains(QUALIFICATION_RATES, fps)) {
            m_fps = fps;
        }
    }
    void Viewport::benchmark(unsigned triangles)
    {
        m_benchmark = triangles;
        m_detail.invalidate();
    }
    unsigned Viewport::frameRate(scene3d::RobotDetail detail) const
    {
        if (m_benchmark) {
            return BENCHMARK_FPS;
        }
        if (m_qualification) {
            return m_fps;
        }
        return detail == scene3d::RobotDetail::Interactive ? scene3d::DetailPolicy::INTERACTIVE_MAX_FPS
                                                           : scene3d::LIVE_MAX_FPS;
    }
    void Viewport::requestFrame(scene3d::RobotDetail detail, std::chrono::steady_clock::time_point now)
    {
        if (m_qualification) {
            detail = m_testInteractive ? scene3d::RobotDetail::Interactive : scene3d::RobotDetail::Full;
        }
        const auto us{
            std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count()
        };
        auto frame{ m_pose };
        frame.revision = ++m_revision;
        // Pose freshness comes from telemetry; never hide transport/render age.
        if (!frame.source_time_us || m_qualification) {
            frame.source_time_us = us;
        }
        if (m_qualification) {
            frame.camera.yaw += float(us % ORBIT_PERIOD_US) / ORBIT_PERIOD_US * ORBIT_TURN;
        }
        if (m_benchmark) {
            frame = scene3d::benchmark_snapshot(
              float(us % BENCHMARK_PERIOD_US) / MICROSECONDS_PER_SECOND,
              { .revision = m_revision, .time_us = static_cast<std::uint64_t>(us) });
        }
        scene3d::Frame request_frame{ .scene = m_benchmark ? &scene3d::benchmark_scene(m_benchmark)
                                                           : &scene3d::robot_scene(detail),
                                      .snapshot = frame,
                                      .maximum_fps = frameRate(detail) };
        if (!m_benchmark) {
            for (unsigned i{ 0 }; i < 3; ++i) {
                request_frame.anchors[i] = { .node = int(i + 1),
                                             .point = {},
                                             .value = float(m_joints[i] * joint_units(i)) };
            }
        }
        if (m_service.submit(request_frame)) {
            m_detail.submitted(detail);
        }
    }
    void Viewport::tick()
    {
        if (!m_open) {
            return;
        }
        const auto now{ std::chrono::steady_clock::now() };
        const auto request{ m_detail.request(now) };
        if (request || m_benchmark || m_qualification) {
            requestFrame(request.value_or(scene3d::RobotDetail::Full), now);
        }
        if (const auto image{ m_service.acquire() }) {
            setBitmap(touchgfx::Bitmap(m_bitmaps[image->slot]));
            m_annotations = image->annotations;
            m_badges = layout_badges(m_annotations);
            m_sourceTimeUs = image->source_time_us;
            setVisible(true);
            invalidate();
        }
    }
    bool Viewport::posePending() const
    {
        for (unsigned i{ 0 }; i < 3; ++i) {
            if (m_annotations[i].value != float(m_joints[i] * joint_units(i))) {
                return true;
            }
        }
        return false;
    }
    void Viewport::annotationState(bool valid, bool fresh)
    {
        if (m_valid != valid || m_fresh != fresh) {
            m_valid = valid;
            m_fresh = fresh;
            for (auto rect : m_badges) {
                invalidateRect(rect);
            }
        }
    }
    void Viewport::draw(const touchgfx::Rect& area) const
    {
        touchgfx::Image::draw(area);
        const auto origin{ getAbsoluteRect() };
        auto fill{ [&](touchgfx::Rect r, std::uint32_t color) {
            r &= area;
            if (!r.isEmpty()) {
                r.x = static_cast<std::int16_t>(r.x + origin.x);
                r.y = static_cast<std::int16_t>(r.y + origin.y);
                touchgfx::HAL::lcd().fillRect(r, color, OPAQUE_ALPHA);
            }
        } };
        constexpr std::array names{ "Axis 1", "Axis 2", "Axis 3" };
        for (unsigned i{ 0 }; i < 3; ++i) {
            const auto& a{ m_annotations[i] };
            if (!a.visible) {
                continue;
            }
            const auto box{ m_badges[i] };
            const int end_x{ box.x + (box.width / 2) };
            const int end_y{ box.y + (box.height / 2) };
            fill({ int16_t(std::min<int>(a.x, end_x)), a.y, int16_t(std::max(1, std::abs(a.x - end_x))), 1 },
                 theme::BORDER);
            fill({ int16_t(end_x),
                   int16_t(std::min<int>(a.y, end_y)),
                   1,
                   int16_t(std::max(1, std::abs(a.y - end_y))) },
                 theme::BORDER);
            fill({ int16_t(a.x - 2), int16_t(a.y - 2), ANCHOR_MARKER_SIZE, ANCHOR_MARKER_SIZE },
                 theme::ACCENT);
            Panel panel{};
            panel.color = BADGE_COLOR;
            panel.radius = BADGE_RADIUS;
            panel.setPosition(static_cast<std::int16_t>(origin.x + box.x),
                              static_cast<std::int16_t>(origin.y + box.y),
                              static_cast<std::int16_t>(box.width),
                              static_cast<std::int16_t>(box.height));
            auto clip{ area & box };
            clip.x = static_cast<std::int16_t>(clip.x - box.x);
            clip.y = static_cast<std::int16_t>(clip.y - box.y);
            if (!clip.isEmpty()) {
                panel.draw(clip);
            }
            ControlLabel label{ true };
            label.setPosition(static_cast<std::int16_t>(origin.x + box.x + BADGE_TEXT_PADDING),
                              static_cast<std::int16_t>(origin.y + box.y + 4),
                              static_cast<std::int16_t>(box.width - (2 * BADGE_TEXT_PADDING)),
                              BADGE_TEXT_HEIGHT);
            label.align(touchgfx::CENTER);
            std::array<char, BADGE_TEXT_BYTES> text{};
            if (m_valid) {
                format(text, "{}\n{:.1f} {}", names[i], double(a.value), i == 2 ? "mm" : "°");
            }
            else {
                format(text, "{}\n--", names[i]);
            }
            label.text(text.data());
            label.setColor(m_fresh ? theme::TEXT : theme::AMBER);
            clip = area;
            clip.x = static_cast<std::int16_t>(clip.x + origin.x);
            clip.y = static_cast<std::int16_t>(clip.y + origin.y);
            clip &= label.getRect();
            clip.x = static_cast<std::int16_t>(clip.x - label.getX());
            clip.y = static_cast<std::int16_t>(clip.y - label.getY());
            if (!clip.isEmpty()) {
                label.draw(clip);
            }
        }
    }
    void Viewport::handleClickEvent(const touchgfx::ClickEvent& event)
    {
        if (event.getType() == touchgfx::ClickEvent::RELEASED && m_pressedBadge >= 0 && m_select) {
            const auto i{ unsigned(m_pressedBadge) };
            if (m_badges[i].intersect({ event.getX(), event.getY(), 1, 1 })) {
                m_select(m_context, i);
            }
        }
        m_dragging = event.getType() == touchgfx::ClickEvent::PRESSED;
        m_pressedBadge = -1;
        if (m_dragging) {
            for (unsigned i{ 0 }; i < 3; ++i) {
                if (m_annotations[i].visible && m_badges[i].intersect({ event.getX(), event.getY(), 1, 1 })) {
                    m_pressedBadge = int(i);
                }
            }
        }
    }
    void Viewport::handleDragEvent(const touchgfx::DragEvent& event)
    {
        m_pressedBadge = -1;
        if (!m_dragging) {
            return;
        }
        const auto yaw{ m_pose.camera.yaw - (static_cast<float>(event.getDeltaX()) * ORBIT_SENSITIVITY) };
        const auto pitch{ std::clamp(m_pose.camera.pitch +
                                       (static_cast<float>(event.getDeltaY()) * ORBIT_SENSITIVITY),
                                     PITCH_MIN,
                                     PITCH_MAX) };
        if (yaw != m_pose.camera.yaw || pitch != m_pose.camera.pitch) {
            m_pose.camera.yaw = yaw;
            m_pose.camera.pitch = pitch;
            m_detail.changed(std::chrono::steady_clock::now());
        }
    }
}
