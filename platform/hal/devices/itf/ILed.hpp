#pragma once

namespace hal::device
{
    class ILed
    {
      public:
        virtual ~ILed() = default;

        ILed(const ILed&) = delete;
        ILed& operator=(const ILed&) = delete;
        ILed(ILed&&) = delete;
        ILed& operator=(ILed&&) = delete;

        virtual auto turnOn() noexcept -> void = 0;
        virtual auto turnOff() noexcept -> void = 0;
        virtual auto toggle() noexcept -> void = 0;
        [[nodiscard]] virtual auto isOn() const noexcept -> bool = 0;

      protected:
        ILed() = default;
    };
}
