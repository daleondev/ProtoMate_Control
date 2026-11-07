#pragma once
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
        void zoom(float factor);
        void reset();
        void rate(unsigned fps);
        void benchmark(unsigned triangles);
        void qualification(unsigned fps)
        {
            rate(fps);
            m_qualification = fps != 0;
            m_dirty = true;
        }
        void handleClickEvent(const touchgfx::ClickEvent&) override;
        void handleDragEvent(const touchgfx::DragEvent&) override;

      private:
        scene3d::RenderService& m_service;
        scene3d::SceneSnapshot m_pose;
        std::array<double, 3> m_joints{};
        alignas(32) std::array<std::uint16_t, 512> m_cache{};
        std::array<touchgfx::BitmapId, 2> m_bitmaps{};
        bool m_qualification{};
        bool m_registered{}, m_open{}, m_dirty{ true }, m_dragging{};
        unsigned m_benchmark{}, m_fps{ 2 };
        std::uint64_t m_revision{};
    };
}
