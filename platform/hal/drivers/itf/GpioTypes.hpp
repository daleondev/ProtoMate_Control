#pragma once

#include <cstdint>
#include <optional>

namespace hal::gpio
{
    enum class Port : std::uint8_t
    {
        A,
        B,
        C,
        D,
        E,
        F,
        G,
        H,
        I,
        J,
        K
    };

    struct Pin
    {
        Port port;
        std::uint8_t number;

        constexpr bool operator==(const Pin&) const = default;
    };

    enum class Level : std::uint8_t
    {
        Low,
        High
    };

    enum class Pull : std::uint8_t
    {
        None,
        Up,
        Down
    };

    enum class Edge : std::uint8_t
    {
        None,
        Rising,
        Falling,
        Both
    };

    enum class OutputType : std::uint8_t
    {
        PushPull,
        OpenDrain
    };

    enum class Speed : std::uint8_t
    {
        Low,
        Medium,
        High,
        VeryHigh
    };

    struct InputConfiguration
    {
        Pin pin;
        Pull pull{ Pull::None };
        Edge edge{ Edge::None };
        // STM32 peripheral input AF0..AF15; must not be combined with EXTI edges.
        // Linux reserves the pin but does not emulate the connected peripheral.
        std::optional<std::uint8_t> alternate_function{};

        constexpr bool operator==(const InputConfiguration&) const = default;
    };

    struct OutputConfiguration
    {
        Pin pin;
        Level initial_level{ Level::Low };
        OutputType type{ OutputType::PushPull };
        Pull pull{ Pull::None };
        Speed speed{ Speed::Low };
        // STM32 peripheral output AF0..AF15. The peripheral drives the pin;
        // write()/toggle() only change the GPIO latch while an AF is selected.
        std::optional<std::uint8_t> alternate_function{};

        constexpr bool operator==(const OutputConfiguration&) const = default;
    };

    [[nodiscard]] constexpr auto inverted(Level level) noexcept -> Level
    {
        return level == Level::High ? Level::Low : Level::High;
    }
}
