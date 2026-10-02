#ifndef RUNTIME_SEMAPHORE_BASE_H
#define RUNTIME_SEMAPHORE_BASE_H

#include "libstdcxx/backend.hpp"

#include <bits/chrono.h>

#include <cerrno>
#include <cstddef>
#include <exception>
#include <limits>

namespace std _GLIBCXX_VISIBILITY(default)
{
    _GLIBCXX_BEGIN_NAMESPACE_VERSION

    class __threadx_semaphore
    {
      public:
        static constexpr ptrdiff_t _S_max{ numeric_limits<int>::max() };

        explicit __threadx_semaphore(ptrdiff_t count) noexcept
        {
            if (count < 0 ||
                runtime::detail::semaphore_init(&m_semaphore, static_cast<unsigned int>(count)) != 0) {
                std::terminate();
            }
        }

        __threadx_semaphore(const __threadx_semaphore&) = delete;
        __threadx_semaphore& operator=(const __threadx_semaphore&) = delete;

        ~__threadx_semaphore()
        {
            if (runtime::detail::semaphore_destroy(&m_semaphore) != 0) {
                std::terminate();
            }
        }

        void _M_acquire() noexcept
        {
            if (runtime::detail::semaphore_wait(&m_semaphore) != 0) {
                std::terminate();
            }
        }

        [[nodiscard]] bool _M_try_acquire() noexcept
        {
            const int status{ runtime::detail::semaphore_try_wait(&m_semaphore) };
            if (status != 0 && status != EAGAIN) {
                std::terminate();
            }
            return status == 0;
        }

        ptrdiff_t _M_release(ptrdiff_t update) noexcept
        {
            while (update-- > 0) {
                if (runtime::detail::semaphore_post(&m_semaphore) != 0) {
                    std::terminate();
                }
            }
            // GCC 16 uses the old value only to assert release's documented
            // precondition. For every conforming call, zero satisfies that
            // check; ThreadX does not expose its prior count atomically.
            return 0;
        }

        template<typename Rep, typename Period>
        [[nodiscard]] bool _M_try_acquire_for(const chrono::duration<Rep, Period>& duration)
        {
            return _M_try_acquire_until(chrono::steady_clock::now() + duration);
        }

        template<typename Clock, typename Duration>
        [[nodiscard]] bool _M_try_acquire_until(const chrono::time_point<Clock, Duration>& deadline)
        {
            if constexpr (is_same_v<Clock, chrono::system_clock>) {
                const auto seconds{ chrono::time_point_cast<chrono::seconds>(deadline) };
                const auto nanoseconds{ chrono::duration_cast<chrono::nanoseconds>(deadline - seconds) };
                const runtime::detail::TimePoint native_deadline{
                    .tv_sec = static_cast<time_t>(seconds.time_since_epoch().count()),
                    .tv_nsec = static_cast<long>(nanoseconds.count())
                };
                const int status{ runtime::detail::semaphore_timed_wait(&m_semaphore, &native_deadline) };
                if (status != 0 && status != ETIMEDOUT) {
                    std::terminate();
                }
                return status == 0;
            }
            else {
                const auto remaining{ deadline - Clock::now() };
                if (remaining <= Duration::zero()) {
                    return _M_try_acquire();
                }
                return _M_try_acquire_until(chrono::system_clock::now() + remaining);
            }
        }

      private:
        runtime::detail::SemaphoreHandle m_semaphore{};
    };

    template<ptrdiff_t>
    using _Semaphore_impl = __threadx_semaphore;

    _GLIBCXX_END_NAMESPACE_VERSION
} // namespace std

#endif // RUNTIME_SEMAPHORE_BASE_H
