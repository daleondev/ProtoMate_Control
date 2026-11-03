#pragma once

#include "Trajectory.hpp"
#include <optional>

namespace motion::trajectory
{
    struct Pulse
    {
        std::uint64_t tick{}; // Absolute execution time, never a rounded period sum.
        std::uint8_t axis{};
        bool forward{};
        constexpr bool operator==(const Pulse&) const = default;
    };

    // Allocation-free numerical edge iterator. Owned by a preparation worker,
    // never called from a DMA ISR. Carry it across segment and upload boundaries.
    // Nearest-cell crossings bound the ideal position error to half a step.
    // Quantized timing adds at most one execution tick of position error.
    class PulseCursor final
    {
      public:
        PulseCursor(const Capabilities& capabilities, Counts position, std::uint64_t origin=0);
        [[nodiscard]] Error append(const Segment& segment) noexcept;
        [[nodiscard]] std::expected<std::optional<Pulse>,Error> next() noexcept;
        [[nodiscard]] Counts position() const noexcept { return m_position; }
        [[nodiscard]] std::uint64_t endTick() const noexcept { return m_end; }
        [[nodiscard]] Error error() const noexcept { return m_error; }
      private:
        struct Axis
        {
            std::array<double,7> divisions{};
            std::size_t count{},interval{};
            std::optional<Pulse> pending;
            std::uint64_t last_tick{};
            bool has_pulsed{},direction{};
        };
        [[nodiscard]] Error advance(unsigned axis) noexcept;
        Capabilities m_capabilities;
        Counts m_position;
        std::array<Axis,3> m_axes{};
        Segment m_segment{};
        MotorStates m_boundary{};
        std::uint64_t m_begin{},m_end{};
        Error m_error{Error::None};
        bool m_active{},m_started{};
    };
}
