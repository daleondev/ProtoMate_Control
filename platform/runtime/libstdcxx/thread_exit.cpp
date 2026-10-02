#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <mutex>

// NOLINTBEGIN(bugprone-reserved-identifier,cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-owning-memory,readability-identifier-naming)
namespace std _GLIBCXX_VISIBILITY(default)
{
    _GLIBCXX_BEGIN_NAMESPACE_VERSION

    namespace
    {
        __gthread_key_t thread_exit_key;

        void run_thread_exit_callbacks(void* pointer)
        {
            auto* element{ static_cast<__at_thread_exit_elt*>(pointer) };
            while (element != nullptr) {
                auto* const next{ element->_M_next };
                element->_M_cb(element);
                element = next;
            }
        }

        void run_current_thread_exit_callbacks()
        {
            auto* const element{
                static_cast<__at_thread_exit_elt*>(__gthread_getspecific(thread_exit_key))
            };
            static_cast<void>(__gthread_setspecific(thread_exit_key, nullptr));
            run_thread_exit_callbacks(element);
        }

        class ThreadExitKey
        {
          public:
            ThreadExitKey()
            {
                if (__gthread_key_create(&thread_exit_key, run_thread_exit_callbacks) != 0) {
                    std::terminate();
                }
            }

            ~ThreadExitKey() { static_cast<void>(__gthread_key_delete(thread_exit_key)); }

            ThreadExitKey(const ThreadExitKey&) = delete;
            ThreadExitKey& operator=(const ThreadExitKey&) = delete;
            ThreadExitKey(ThreadExitKey&&) = delete;
            ThreadExitKey& operator=(ThreadExitKey&&) = delete;
        };

        void initialize_thread_exit_key()
        {
            static const ThreadExitKey key_owner;
            static_cast<void>(key_owner);
            if (std::atexit(run_current_thread_exit_callbacks) != 0) {
                std::terminate();
            }
        }

        struct notifier final : __at_thread_exit_elt
        {
            notifier(condition_variable& condition, unique_lock<mutex>& lock)
              : __at_thread_exit_elt{}, condition{ &condition }, mutex_pointer{ lock.release() }
            {
                _M_cb = &notifier::run;
            }

            ~notifier()
            {
                mutex_pointer->unlock();
                condition->notify_all();
            }

            notifier(const notifier&) = delete;
            notifier& operator=(const notifier&) = delete;
            notifier(notifier&&) = delete;
            notifier& operator=(notifier&&) = delete;

            static void run(void* pointer) { delete static_cast<notifier*>(pointer); }

            condition_variable* condition;
            mutex* mutex_pointer;
        };
    }

    void __at_thread_exit(__at_thread_exit_elt* element)
    {
        static __gthread_once_t once = __GTHREAD_ONCE_INIT;
        if (__gthread_once(&once, initialize_thread_exit_key) != 0) {
            std::terminate();
        }

        element->_M_next =
          static_cast<__at_thread_exit_elt*>(__gthread_getspecific(thread_exit_key));
        if (__gthread_setspecific(thread_exit_key, element) != 0) {
            std::terminate();
        }
    }

    void notify_all_at_thread_exit(condition_variable& condition, unique_lock<mutex> lock)
    {
        auto* const notification{ new notifier{ condition, lock } };
        __at_thread_exit(notification);
    }

    _GLIBCXX_END_NAMESPACE_VERSION
}
// NOLINTEND(bugprone-reserved-identifier,cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-owning-memory,readability-identifier-naming)
