#pragma once
#include "Trace.h"

namespace profiling
{
    inline void interruptEnter() noexcept
    {
#if defined(PROTOMATE_PROFILING) && defined(__arm__)
        unsigned irq;
        asm volatile("mrs %0, ipsr" : "=r"(irq));
        profile_irq_enter(irq);
#endif
    }
    inline void interruptExit() noexcept
    {
#if defined(PROTOMATE_PROFILING) && defined(__arm__)
        unsigned irq;
        asm volatile("mrs %0, ipsr" : "=r"(irq));
        profile_irq_exit(irq);
#endif
    }
    class Scope final
    {
      public:
#if defined(PROTOMATE_PROFILING)
        explicit Scope(unsigned site, unsigned category = PROFILE_CAT_SYSTEM) noexcept
          : m_site{ site }
          , m_category{ category }
          , m_generation{ profile_generation() }
          , m_time{ profile_time_us() }
          , m_cycles{ profile_cycles() }
        {
        }
        ~Scope()
        {
            if (m_generation)
                profile_measure(
                  m_site, profile_cycles() - m_cycles, profile_time_us() - m_time, m_category, m_generation);
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

      private:
        unsigned m_site, m_category, m_generation, m_time, m_cycles;
#else
        explicit Scope(unsigned, unsigned = PROFILE_CAT_SYSTEM) noexcept {}
#endif
    };
}
