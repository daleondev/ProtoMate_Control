#include <future>

// libstdc++'s Arm "single" thread model omits the shared-state ABI objects
// used by promise, future, packaged_task, and async.
// NOLINTBEGIN(bugprone-reserved-identifier,readability-identifier-naming)
namespace std _GLIBCXX_VISIBILITY(default)
{
    _GLIBCXX_BEGIN_NAMESPACE_VERSION

    __future_base::_Result_base::_Result_base() = default;
    __future_base::_Result_base::~_Result_base() = default;

    void __future_base::_State_baseV2::_Make_ready::_S_run(void* pointer)
    {
        unique_ptr<_Make_ready> ready{ static_cast<_Make_ready*>(pointer) };
        if (auto state{ ready->_M_shared_state.lock() }) {
            state->_M_status._M_store_notify_all(_Status::__ready, memory_order_release);
        }
    }

    extern void __at_thread_exit(__at_thread_exit_elt* element);

    void __future_base::_State_baseV2::_Make_ready::_M_set()
    {
        _M_cb = &_Make_ready::_S_run;
        __at_thread_exit(this);
    }

    _GLIBCXX_END_NAMESPACE_VERSION
}
// NOLINTEND(bugprone-reserved-identifier,readability-identifier-naming)
