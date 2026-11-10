#pragma once
#include "simulation/SharedMemory.hpp"
#include <functional>
namespace simulation
{
    class Peripherals final
    {
      public:
        Peripherals(const Peripherals&) = delete;
        Peripherals& operator=(const Peripherals&) = delete;
        Peripherals(Peripherals&&) = delete;
        Peripherals& operator=(Peripherals&&) = delete;
        ~Peripherals() = default;

        using Output = std::function<void(unsigned, bool, std::uint64_t)>;
        Peripherals(SharedMemory& shared, Output output)
          : m_shared{ shared }
          , m_output{ std::move(output) }
        {
        }
        void execute(const Packet& q, Packet& r, std::uint64_t now);
        std::uint64_t deadline(std::uint64_t now, std::uint64_t maximum) const;
        unsigned advance(std::uint64_t now);
        void stop(std::uint64_t now) noexcept;

      private:
        void executePwm(const Packet& q, Packet& r, std::uint64_t now);
        struct Timer
        {
            std::uint64_t origin{}, cycles{}, notifications{};
            std::uint32_t frequency{}, prescaler{}, reload{}, counter{}, version{};
            bool running{}, interrupt{};
        };
        static constexpr unsigned TIMER_COUNT{ 18 };
        std::array<Timer, TIMER_COUNT> m_timers{};
        struct Pwm
        {
            unsigned pin{};
            std::uint64_t period{}, high{}, next{}, count{}, limit{};
            bool configured{}, running{}, level{}, counted{};
        };
        static bool startPwm(Pwm& /*p*/, const Packet& /*q*/, std::uint64_t now);
        static bool configureTimer(Timer& /*t*/, const Packet& /*q*/, std::uint64_t now);
        std::array<Pwm, 3> m_pwm{};
        SharedMemory& m_shared;
        Output m_output;
        static std::uint64_t elapsed(const Timer& t, std::uint64_t now);
        static void capture(Timer& t, std::uint64_t now);
        struct Publication
        {
            unsigned index;
            std::uint64_t time_ns;
        };
        void publish(Publication publication);
    };
}
