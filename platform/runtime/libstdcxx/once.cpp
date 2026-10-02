#include <functional>
#include <mutex>
#include <utility>

// libstdc++ omits these ABI objects when built with the Arm "single" thread
// model. The definitions mirror libstdc++'s mutex.cc while using our gthread
// implementation underneath.
// NOLINTBEGIN(bugprone-reserved-identifier,cppcoreguidelines-avoid-non-const-global-variables,readability-identifier-naming)
namespace std _GLIBCXX_VISIBILITY(default)
{
    _GLIBCXX_BEGIN_NAMESPACE_VERSION

#if defined(_GLIBCXX_HAVE_TLS)
    __thread void* __once_callable;
    __thread void (*__once_call)();

    extern "C" void __once_proxy() { __once_call(); }
#else
    function<void()> __once_functor;

    mutex& __get_once_mutex()
    {
        static mutex once_mutex;
        return once_mutex;
    }

    namespace
    {
        unique_lock<mutex>* set_lock_pointer(unique_lock<mutex>* pointer)
        {
            static unique_lock<mutex>* current{};
            return std::exchange(current, pointer);
        }
    }

    void __set_once_functor_lock_ptr(unique_lock<mutex>* pointer)
    {
        static_cast<void>(set_lock_pointer(pointer));
    }

    unique_lock<mutex>& __get_once_functor_lock()
    {
        static unique_lock<mutex> lock{ __get_once_mutex(), defer_lock };
        return lock;
    }

    extern "C" void __once_proxy()
    {
        function<void()> callable{ std::move(__once_functor) };
        if (unique_lock<mutex>* lock{ set_lock_pointer(nullptr) }; lock != nullptr) {
            lock->unlock();
        }
        else {
            __get_once_functor_lock().unlock();
        }
        callable();
    }
#endif

    _GLIBCXX_END_NAMESPACE_VERSION
}
// NOLINTEND(bugprone-reserved-identifier,cppcoreguidelines-avoid-non-const-global-variables,readability-identifier-naming)
