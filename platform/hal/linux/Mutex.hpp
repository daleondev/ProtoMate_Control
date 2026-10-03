#pragma once

#include <exception>
#include <pthread.h>

namespace hal::linux
{
    // The Linux HAL sits below the ThreadX C++ runtime. std::mutex has a
    // different layout in native and ThreadX translation units; never embed
    // it in a HAL type also included by applications/tests. Use one explicit
    // native ABI, as the Linux timer/console already do for their locks.
    class Mutex final
    {
      public:
        explicit Mutex(bool recursive = false) noexcept
        {
            pthread_mutexattr_t attributes;
            if (pthread_mutexattr_init(&attributes) != 0) {
                std::terminate();
            }
            if (recursive && pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE) != 0) {
                std::terminate();
            }
            if (pthread_mutex_init(&m_mutex, &attributes) != 0) {
                std::terminate();
            }
            pthread_mutexattr_destroy(&attributes);
        }
        ~Mutex()
        {
            if (pthread_mutex_destroy(&m_mutex) != 0) {
                std::terminate();
            }
        }
        Mutex(const Mutex&) = delete;
        Mutex& operator=(const Mutex&) = delete;
        auto lock() noexcept -> void
        {
            if (pthread_mutex_lock(&m_mutex) != 0) {
                std::terminate();
            }
        }
        auto unlock() noexcept -> void
        {
            if (pthread_mutex_unlock(&m_mutex) != 0) {
                std::terminate();
            }
        }

      private:
        pthread_mutex_t m_mutex{};
    };
}
