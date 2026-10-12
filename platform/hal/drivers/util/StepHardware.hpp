#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace hal::util
{
    inline constexpr std::uint32_t STEP_TICK_NS{ 100U };
    inline constexpr std::uint32_t STEP_MIN_PHASE{ 50U };
    // CCR=park is outside CNT's range. RM0433: overflow can set CCIF here,
    // but does not toggle OCREF. Thus even an unserviced final edge stays low.
    inline constexpr std::uint32_t STEP_PARK{ 0xFFFFFFFFU };
    inline constexpr std::uint32_t STEP_ARR{ STEP_PARK - 1U };
    inline constexpr std::uint32_t STEP_HORIZON{ 0x3FFFFFFFU };
    inline constexpr std::size_t STEP_BUFFER_EDGES{ 512U };
    using StepBuffer = std::array<std::uint32_t, STEP_BUFFER_EDGES>;
    using StepBuffers = std::array<std::array<StepBuffer, 2>, 3>;

    [[nodiscard]] constexpr auto step_add(std::uint32_t tick, std::uint32_t delta) noexcept -> std::uint32_t
    {
        // CNT wraps modulo 2^32 - 1, not 2^32. Fold an unsigned carry
        // back into bit zero; avoid a software 64-bit division on Cortex-M4.
        auto sum{ tick + delta };
        if (sum < tick) {
            ++sum;
        }
        return sum == STEP_PARK ? 0U : sum;
    }
    [[nodiscard]] constexpr auto step_distance(std::uint32_t from, std::uint32_t to) noexcept -> std::uint32_t
    {
        return to >= from ? to - from : STEP_PARK - from + to;
    }

    struct StepChannelSample
    {
        std::uint32_t tick{};
        std::uint32_t compare{ STEP_PARK };
        std::uint32_t remaining{};
        std::uint8_t target{};
        bool transfer_complete{};
        bool compare_pending{};
        bool high{};
    };
    struct StepSample
    {
        std::uint32_t tick{};
        std::array<StepChannelSample, 3> channels{};
        bool running{};
        bool error{};
    };

    // Small register boundary shared by the STM32 backend and deterministic
    // host model. DMA owns the active buffer; software only writes its peer.
    class StepHardware
    {
      public:
        StepHardware() = default;
        StepHardware(const StepHardware&) = default;
        auto operator=(const StepHardware&) -> StepHardware& = default;
        StepHardware(StepHardware&&) = default;
        auto operator=(StepHardware&&) -> StepHardware& = default;

        virtual ~StepHardware() = default;
        virtual auto buffers() noexcept -> StepBuffers& = 0;
        virtual auto reset() noexcept -> bool = 0;
        // Buffers are ready before arming a single channel on a running timer.
        // False means the first deadline became too close; no pulse was emitted.
        virtual auto arm(std::size_t axis, std::uint32_t first, std::uint32_t entries) noexcept -> bool = 0;
        virtual auto start(std::uint32_t guard_tick) noexcept -> void = 0;
        virtual auto guard(std::uint32_t tick) noexcept -> void = 0;
        // A finite tail may park before DMA HT/TC. Poll it every 1 ms using
        // TIM7 while needed; this never schedules STEP edges or rearms pulses.
        virtual auto completionWatch(bool enabled) noexcept -> void = 0;
        virtual auto sample() noexcept -> StepSample = 0;
        virtual auto acknowledge(std::size_t axis) noexcept -> void = 0;
        virtual auto finishAxis(std::size_t axis) noexcept -> void = 0;
        // Settle this channel's DMA, park its compare and sample its final
        // phase/count BEFORE forcing low. The counter/other channels keep running.
        virtual auto stopAxis(std::size_t axis) noexcept -> StepSample = 0;
        // Freeze the counter, settle DMA, capture progress, then force pins low.
        virtual auto stop() noexcept -> StepSample = 0;
        // Stop/join any autonomous host service BEFORE acquiring the generator lock.
        virtual auto stopService() noexcept -> void {}
        // Called while the owning generator is still alive and locked. Prevent
        // pending IRQs from dispatching into partially destroyed state.
        virtual auto shutdown() noexcept -> void { static_cast<void>(stop()); }
        // Publish CPU buffer writes before exposing the extended deadline.
        virtual auto publish() noexcept -> void = 0;
    };
}
