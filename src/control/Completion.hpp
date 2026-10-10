#pragma once
#include <functional>
#include <future>
#include <mutex>
#include <optional>

namespace control
{
    // One observer installed by the execution owner. Registration after completion
    // delivers immediately. Callbacks only enqueue notifications, never re-enter a
    // motor/controller: completion can occur while its command lock is held.
    template<typename T>
    class Completion
    {
        struct State
        {
            std::mutex mutex;
            std::promise<T> promise;
            std::optional<T> value;
            std::exception_ptr error;
            std::function<void()> callback;
        };
        std::shared_ptr<State> m_state = std::make_shared<State>();

      public:
        bool operator==(const Completion& other) const noexcept { return m_state == other.m_state; }
        std::future<T> getFuture() { return m_state->promise.get_future(); }
        void setValue(T value)
        {
            std::function<void()> callback;
            {
                std::scoped_lock lock(m_state->mutex);
                m_state->promise.set_value(value);
                m_state->value = value;
                callback = std::move(m_state->callback);
            }
            if (callback)
                callback();
        }
        void setException(const std::exception_ptr& error)
        {
            std::function<void()> callback;
            {
                std::scoped_lock lock(m_state->mutex);
                m_state->promise.set_exception(error);
                m_state->error = error;
                callback = std::move(m_state->callback);
            }
            if (callback)
                callback();
        }
        void then(const std::function<void()>& callback) const
        {
            bool ready{};
            {
                std::scoped_lock lock(m_state->mutex);
                ready = m_state->value.has_value() || bool(m_state->error);
                if (!ready)
                    m_state->callback = callback;
            }
            if (ready && callback)
                callback();
        }
        std::optional<T> result() const
        {
            std::scoped_lock lock(m_state->mutex);
            if (m_state->error)
                std::rethrow_exception(m_state->error);
            return m_state->value;
        }
    };
}
