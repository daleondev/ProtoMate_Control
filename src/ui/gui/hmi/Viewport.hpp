#pragma once
#include "ui/gui/scene3d/DetailPolicy.hpp"
#include "ui/gui/scene3d/RenderService.hpp"
#include <touchgfx/events/ClickEvent.hpp>
#include <touchgfx/events/DragEvent.hpp>
#include <touchgfx/widgets/Image.hpp>

namespace ui::gui::hmi
{
    class Viewport final : public touchgfx::Image
    {
      public:
        Viewport(const Viewport&) = delete;
        auto operator=(const Viewport&) -> Viewport& = delete;
        Viewport(Viewport&&) = delete;
        auto operator=(Viewport&&) -> Viewport& = delete;

        explicit Viewport(scene3d::RenderService& service);
        ~Viewport() override;
        void open();
        void close();
        void pose(const std::array<double, 3>& joints, std::uint64_t source_us);
        void tick();
        bool posePending() const;
        std::uint64_t sourceTimeUs() const { return m_sourceTimeUs; }
        void zoom(float factor);
        void reset();
        void annotationState(bool valid, bool fresh);
        void selection(void (*cb)(void*, unsigned), void* context)
        {
            m_select = cb;
            m_context = context;
        }
        void draw(const touchgfx::Rect& /*invalidatedArea*/) const override;
        void rate(unsigned fps);
        void benchmark(unsigned triangles);
        void qualification(unsigned fps, bool interactive = false)
        {
            rate(fps);
            m_qualification = fps != 0;
            m_testInteractive = interactive;
            m_detail.invalidate();
        }
        void handleClickEvent(const touchgfx::ClickEvent& /*event*/) override;
        void handleDragEvent(const touchgfx::DragEvent& /*event*/) override;

      private:
        static constexpr unsigned CACHE_ALIGNMENT{ 32 }, BITMAP_CACHE_WORDS{ 512 };
        void unregisterBitmaps() noexcept;
        unsigned frameRate(scene3d::RobotDetail detail) const;
        void requestFrame(scene3d::RobotDetail detail, std::chrono::steady_clock::time_point now);
        std::array<scene3d::ProjectedAnchor, 3> m_annotations{};
        std::array<touchgfx::Rect, 3> m_badges{};
        bool m_valid{}, m_fresh{};
        int m_pressedBadge{ -1 };
        void (*m_select)(void*, unsigned){};
        void* m_context{};
        scene3d::RenderService& m_service;
        scene3d::SceneSnapshot m_pose;
        std::array<double, 3> m_joints{};
        alignas(CACHE_ALIGNMENT) std::array<std::uint16_t, BITMAP_CACHE_WORDS> m_cache{};
        std::array<touchgfx::BitmapId, 2> m_bitmaps{};
        bool m_qualification{};
        bool m_registered{}, m_open{}, m_dragging{}, m_testInteractive{};
        scene3d::DetailPolicy m_detail;
        unsigned m_benchmark{}, m_fps{ 2 };
        std::uint64_t m_revision{}, m_sourceTimeUs{};
    };
}
