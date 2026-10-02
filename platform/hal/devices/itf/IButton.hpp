#pragma once

#include <functional>

namespace hal::device
{
    class IButton
    {
      public:
        enum class State
        {
            Released,
            Pressed
        };

        using StateChangedCallback = std::move_only_function<void(State) noexcept>;

        virtual ~IButton() = default;

        IButton(const IButton&) = delete;
        IButton& operator=(const IButton&) = delete;
        IButton(IButton&&) = delete;
        IButton& operator=(IButton&&) = delete;

        [[nodiscard]] virtual auto state() const noexcept -> State = 0;
        [[nodiscard]] auto isPressed() const noexcept -> bool { return state() == State::Pressed; }

        virtual auto setStateChangedCallback(StateChangedCallback callback) noexcept -> void = 0;
        auto clearStateChangedCallback() noexcept -> void { setStateChangedCallback({}); }

      protected:
        IButton() = default;
    };
}
