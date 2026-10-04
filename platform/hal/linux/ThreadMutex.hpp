#pragma once
#include <exception>
#include <tx_api.h>

namespace hal::linux
{
    // Explicit C ABI across native-HAL and ThreadX-libstdc++ translation units.
    // Unlike a native pthread mutex, contention suspends the ThreadX caller.
    class ThreadMutex final
    {
      public:
        ThreadMutex()
        {
            if (tx_mutex_create(&m_mutex, const_cast<char*>("step generator"), TX_INHERIT) != TX_SUCCESS)
                std::terminate();
        }
        ~ThreadMutex()
        {
            if (tx_mutex_delete(&m_mutex) != TX_SUCCESS)
                std::terminate();
        }
        ThreadMutex(const ThreadMutex&) = delete;
        ThreadMutex& operator=(const ThreadMutex&) = delete;
        void lock()
        {
            if (tx_mutex_get(&m_mutex, TX_WAIT_FOREVER) != TX_SUCCESS)
                std::terminate();
        }
        void unlock()
        {
            if (tx_mutex_put(&m_mutex) != TX_SUCCESS)
                std::terminate();
        }

      private:
        TX_MUTEX m_mutex{};
    };
}
