#include <bits/functexcept.h>

#include <condition_variable>
#include <mutex>

#include <cerrno>

#if !defined(THREADX_STD_ENABLED)
#error "The standard library must use the ThreadX gthread port"
#endif

namespace std
{
    condition_variable::condition_variable() noexcept = default;
    condition_variable::~condition_variable() noexcept = default;

    void condition_variable::notify_one() noexcept { _M_cond.notify_one(); }
    void condition_variable::notify_all() noexcept { _M_cond.notify_all(); }

    void condition_variable::wait(unique_lock<mutex>& lock)
    {
        if (!lock.owns_lock() || lock.mutex() == nullptr) {
            std::__throw_system_error(EPERM);
        }
        _M_cond.wait(*lock.mutex());
    }
}
