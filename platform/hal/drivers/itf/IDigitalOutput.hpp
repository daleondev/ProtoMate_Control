#pragma once

#include "hal/drivers/itf/IGpio.hpp"

namespace hal
{
    class IDigitalOutput
    {
      public:
        virtual ~IDigitalOutput() = default;

        IDigitalOutput(const IDigitalOutput&) = delete;
        IDigitalOutput& operator=(const IDigitalOutput&) = delete;
        IDigitalOutput(IDigitalOutput&&) = delete;
        IDigitalOutput& operator=(IDigitalOutput&&) = delete;

        [[nodiscard]] virtual auto read() const noexcept -> gpio::Level = 0;
        virtual auto write(gpio::Level level) noexcept -> void = 0;
        virtual auto toggle() noexcept -> void = 0;

      protected:
        IDigitalOutput() = default;
    };
}
