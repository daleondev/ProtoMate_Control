#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>

namespace app
{
    // Application lifecycle, independent of motor enable, referencing and motion.
    enum class State : std::uint8_t
    {
        Starting,
        Ready,
        Stopping,
        Stopped,
        Fault
    };

    inline constexpr std::chrono::milliseconds STATE_PERIOD{ 100 };
    inline constexpr std::chrono::milliseconds STATE_MAX_AGE{ 500 };

    // An orderly transport shutdown must not hide a latched worker/system fault.
    constexpr int exitStatus(std::initializer_list<State> states, bool connection_failed = false) noexcept
    {
        for (const auto state : states) {
            if (state == State::Fault) {
                return EXIT_FAILURE;
            }
        }
        return connection_failed ? EXIT_FAILURE : EXIT_SUCCESS;
    }

    // One-shot lifecycle. A late worker cannot overwrite shutdown or a fault.
    class ControllerState final
    {
      public:
        [[nodiscard]] State get() const noexcept { return m_state.load(); }
        void ready() noexcept
        {
            auto expected{ State::Starting };
            m_state.compare_exchange_strong(expected, State::Ready);
        }
        void stopping() noexcept
        {
            auto current{ m_state.load() };
            while (current == State::Starting || current == State::Ready) {
                if (m_state.compare_exchange_weak(current, State::Stopping)) {
                    break;
                }
            }
        }
        void stopped() noexcept
        {
            auto current{ m_state.load() };
            while (current != State::Fault && current != State::Stopped) {
                if (m_state.compare_exchange_weak(current, State::Stopped)) {
                    break;
                }
            }
        }
        void fail() noexcept { m_state.store(State::Fault); }

      private:
        std::atomic<State> m_state{ State::Starting };
    };
}
