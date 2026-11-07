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
        explicit Viewport(scene3d::RenderService& service);
        ~Viewport() override;
        void open();
        void close();
        void pose(const std::array<double, 3>& joints, std::uint64_t sourceUs);
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
        void draw(const touchgfx::Rect&) const override;
        void rate(unsigned fps);
        void benchmark(unsigned triangles);
        void qualification(unsigned fps, bool interactive = false)
        {
            rate(fps);
            m_qualification = fps != 0;
            m_testInteractive = interactive;
            m_detail.invalidate();
        }
        void handleClickEvent(const touchgfx::ClickEvent&) override;
        void handleDragEvent(const touchgfx::DragEvent&) override;

      private:
        std::array<scene3d::ProjectedAnchor, 3> m_annotations{};
        std::array<touchgfx::Rect, 3> m_badges{};
        bool m_valid{}, m_fresh{};
        int m_pressedBadge{ -1 };
        void (*m_select)(void*, unsigned){};
        void* m_context{};
        scene3d::RenderService& m_service;
        scene3d::SceneSnapshot m_pose;
        std::array<double, 3> m_joints{};
        alignas(32) std::array<std::uint16_t, 512> m_cache{};
        std::array<touchgfx::BitmapId, 2> m_bitmaps{};
        bool m_qualification{};
        bool m_registered{}, m_open{}, m_dragging{}, m_testInteractive{};
        scene3d::DetailPolicy m_detail;
        unsigned m_benchmark{}, m_fps{ 2 };
        std::uint64_t m_revision{}, m_sourceTimeUs{};
    };
}
