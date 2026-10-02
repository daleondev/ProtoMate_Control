#include "Rng.hpp"

#include "hal/drivers/common.hpp"

#include <exception>

namespace
{
    CHAR rng_mutex_name[]{ "Hardware RNG" };

    class ThreadXLock final
    {
      public:
        explicit ThreadXLock(TX_MUTEX& mutex)
          : m_mutex{ mutex }
        {
            if (tx_mutex_get(&m_mutex, TX_WAIT_FOREVER) != TX_SUCCESS) {
                std::terminate();
            }
        }

        ~ThreadXLock()
        {
            if (tx_mutex_put(&m_mutex) != TX_SUCCESS) {
                std::terminate();
            }
        }

        ThreadXLock(const ThreadXLock&) = delete;
        ThreadXLock& operator=(const ThreadXLock&) = delete;
        ThreadXLock(ThreadXLock&&) = delete;
        ThreadXLock& operator=(ThreadXLock&&) = delete;

      private:
        TX_MUTEX& m_mutex;
    };
}

namespace hal
{
    Rng::Rng(RNG_HandleTypeDef& handle)
      : m_handle{ handle }
    {
        if (tx_mutex_create(&m_mutex, rng_mutex_name, TX_INHERIT) != TX_SUCCESS) {
            std::terminate();
        }
    }

    Rng::~Rng()
    {
        if (tx_mutex_delete(&m_mutex) != TX_SUCCESS) {
            std::terminate();
        }
    }

    auto Rng::generate() noexcept -> util::Result<Value>
    {
        const ThreadXLock lock{ m_mutex };
        Value value{};
        const HAL_StatusTypeDef status{ HAL_RNG_GenerateRandomNumber(&m_handle, &value) };
        if (status != HAL_OK) {
            return make_error_result<Value>(static_cast<HalError>(status));
        }
        return value;
    }
}
