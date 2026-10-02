// NOLINTNEXTLINE(bugprone-reserved-identifier,cppcoreguidelines-macro-usage,readability-identifier-naming)
#define _GLIBCXX_THREAD_IMPL 1

#include "backend.hpp"

#include <bits/functexcept.h>

#include <memory>
#include <thread>
#include <type_traits>

#include <cerrno>

#if !defined(THREADX_STD_ENABLED)
#error "The standard library must use the ThreadX gthread port"
#endif

static_assert(std::is_same_v<__gthread_t, TX_THREAD*>);
static_assert(std::is_same_v<std::thread::native_handle_type, TX_THREAD*>);

namespace
{
    auto run_thread_state(void* state_pointer) noexcept -> void*
    {
        std::unique_ptr<std::thread::_State> state{ static_cast<std::thread::_State*>(state_pointer) };
        try {
            state->_M_run();
        } catch (...) {
            // The C++ standard requires an exception escaping a thread's
            // initial function to terminate the process. Never unwind through
            // the ThreadX C entry frame.
            std::terminate();
        }
        return nullptr;
    }
}

namespace std
{
    thread::_State::~_State() = default;

    void thread::_M_start_thread(_State_ptr state, void (*dependency)())
    {
        auto attributes{ runtime::thread::consume_attributes() };

        static_cast<void>(dependency);
        __gthread_t native_thread{};
        const int error{ runtime::detail::thread_create(
          &native_thread, run_thread_state, state.get(), attributes.value_or({})) };
        if (error != 0) {
            std::__throw_system_error(error);
        }
        _M_id = id(native_thread);
        [[maybe_unused]] auto* const transferred_state{ state.release() };
    }

    void thread::join()
    {
        if (!joinable()) {
            std::__throw_system_error(EINVAL);
        }
        if (get_id() == this_thread::get_id()) {
            std::__throw_system_error(EDEADLK);
        }
        const int error{ __gthread_join(_M_id._M_thread, nullptr) };
        if (error != 0) {
            std::__throw_system_error(error);
        }
        _M_id = id{};
    }

    void thread::detach()
    {
        if (!joinable()) {
            std::__throw_system_error(EINVAL);
        }
        const int error{ __gthread_detach(_M_id._M_thread) };
        if (error != 0) {
            std::__throw_system_error(error);
        }
        _M_id = id{};
    }

    unsigned int thread::hardware_concurrency() noexcept { return 1U; }
}
