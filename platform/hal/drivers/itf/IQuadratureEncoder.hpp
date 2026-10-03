#pragma once

#include "hal/utilities/Result.hpp"

#include <cstdint>

namespace hal
{
    class IQuadratureEncoder
    {
      public:
        using Count = std::int64_t;

        virtual ~IQuadratureEncoder() = default;
        IQuadratureEncoder(const IQuadratureEncoder&) = delete;
        IQuadratureEncoder& operator=(const IQuadratureEncoder&) = delete;

        // x4 quadrature counts, not revolutions or commanded motor steps.
        // Start/stop preserve position; construction is stopped at zero.
        [[nodiscard]] virtual auto start() noexcept -> util::Result<> = 0;
        [[nodiscard]] virtual auto stop() noexcept -> util::Result<> = 0;
        [[nodiscard]] virtual auto isRunning() const noexcept -> bool = 0;
        // Requires the backend's interrupt-service deadline to be met. Errors
        // from ambiguous samples or signed overflow remain latched until reset.
        [[nodiscard]] virtual auto position() const noexcept -> util::Result<Count> = 0;
        // Stopped-only origin change; clears a latched count-extension error.
        [[nodiscard]] virtual auto setPosition(Count count) noexcept -> util::Result<> = 0;
        [[nodiscard]] auto reset() noexcept -> util::Result<> { return setPosition(0); }

      protected:
        IQuadratureEncoder() = default;
    };
}
