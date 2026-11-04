#pragma once
#include "simulation/SharedMemory.hpp"
namespace simulation
{
    class Peripherals final
    {
      public:
        explicit Peripherals(SharedMemory& shared)
          : m_shared{ shared }
        {
        }
        void execute(const Packet& q, Packet& r, std::uint64_t now);
        std::uint64_t deadline(std::uint64_t now, std::uint64_t maximum) const;
        unsigned advance(std::uint64_t now);

      private:
        struct Timer
        {
            std::uint64_t origin{}, cycles{}, notifications{};
            std::uint32_t frequency{}, prescaler{}, reload{}, counter{}, version{};
            bool running{}, interrupt{};
        };
        std::array<Timer, 18> m_timers{};
        struct Pwm
        {
            unsigned pin{};
            std::uint64_t period{}, high{}, next{}, count{}, limit{};
            bool running{}, level{}, counted{};
        };
        std::array<Pwm, 3> m_pwm{};
        SharedMemory& m_shared;
        std::uint64_t elapsed(const Timer& t, std::uint64_t now) const;
        void capture(Timer& t, std::uint64_t now);
        void publish(unsigned index, std::uint64_t now);
    };
}
