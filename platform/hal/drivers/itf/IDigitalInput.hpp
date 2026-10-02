#pragma once

#include "hal/drivers/itf/IGpio.hpp"

#include <functional>

namespace hal
{
    class IDigitalInput
    {
      public:
        using EdgeCallback = std::move_only_function<void(gpio::Level) noexcept>;

        virtual ~IDigitalInput() = default;

        IDigitalInput(const IDigitalInput&) = delete;
        IDigitalInput& operator=(const IDigitalInput&) = delete;
        IDigitalInput(IDigitalInput&&) = delete;
        IDigitalInput& operator=(IDigitalInput&&) = delete;

        [[nodiscard]] virtual auto read() const noexcept -> gpio::Level = 0;
        // On embedded backends this callback executes in interrupt context.
        virtual auto setEdgeCallback(EdgeCallback callback) noexcept -> void = 0;
        auto clearEdgeCallback() noexcept -> void { setEdgeCallback({}); }

      protected:
        IDigitalInput() = default;
    };
}
