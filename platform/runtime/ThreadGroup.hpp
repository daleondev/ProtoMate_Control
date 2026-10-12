#pragma once

#include "thread.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <meta>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

namespace runtime::thread
{
    // An explicit annotation marks an instance method as a thread entry point.
    struct Definition
    {
        std::int32_t priority{};
        std::size_t stack_size{};
    };
}

namespace runtime
{
    // The owner must outlive the group and arrange for its entries to return
    // before destruction. start()/join() are called by one lifecycle owner;
    // stopping the entries is application-specific.
    class ThreadGroup final
    {
      public:
        ThreadGroup() = default;
        ~ThreadGroup() { join(); }
        ThreadGroup(const ThreadGroup&) = delete;
        ThreadGroup& operator=(const ThreadGroup&) = delete;
        ThreadGroup(ThreadGroup&&) = delete;
        ThreadGroup& operator=(ThreadGroup&&) = delete;

        // Friend ThreadGroup to allow reflection of private entry methods.
        // No entry runs until every thread has been created successfully.
        template<typename Owner>
        void start(Owner& owner, std::string_view name)
        {
            if (m_started) {
                throw std::logic_error("Thread group can only be started once");
            }
            m_started = true;
            static constexpr auto entries = std::define_static_array(threadEntries<Owner>());
            constexpr auto count = entries.size();
            m_threads.reserve(count);
            try {
                // Clang generates reserved names for expansion statements.
                // NOLINTBEGIN(bugprone-reserved-identifier,readability-identifier-naming)
                template for (constexpr auto entry : entries) {
                    // NOLINTEND(bugprone-reserved-identifier,readability-identifier-naming)
                    static_assert(std::is_convertible_v<decltype(&[:entry:]), void (Owner::*)()>,
                                  "Thread entries must be nonstatic void methods without arguments");
                    constexpr auto definition = definitionOf(entry);
                    static_assert(definition.priority >= 0 && definition.priority < TX_MAX_PRIORITIES,
                                  "Thread priority is out of range");
                    static_assert(definition.stack_size >= TX_MINIMUM_STACK,
                                  "Thread stack is smaller than the platform minimum");
                    const auto thread_name = std::string(name) + "." + std::string(std::meta::identifier_of(entry));
                    m_threads.push_back(thread::create(
                      { .name = thread_name, .priority = definition.priority, .stack_size = definition.stack_size },
                      [this, &owner] {
                          if (waitForLaunch()) {
                              (owner.*&[:entry:])();
                          }
                      }));
                }
            } catch (...) {
                release(false);
                join();
                throw;
            }
            release(true);
        }

        void join()
        {
            for (auto& worker : m_threads) {
                if (worker.joinable()) {
                    worker.join();
                }
            }
        }

      private:
        static consteval auto definitionsOf(std::meta::info member)
        {
            std::vector<std::meta::info> definitions;
            for (const auto annotation : std::meta::annotations_of(member)) {
                if (std::meta::is_same_type(std::meta::remove_const(std::meta::type_of(annotation)),
                                            ^^thread::Definition)) {
                    definitions.push_back(annotation);
                }
            }
            return definitions;
        }

        template<typename Owner>
        static consteval auto threadEntries()
        {
            std::vector<std::meta::info> entries;
            for (const auto member : std::meta::members_of(^^Owner, std::meta::access_context::current())) {
                if (!definitionsOf(member).empty()) {
                    entries.push_back(member);
                }
            }
            return entries;
        }

        static consteval thread::Definition definitionOf(std::meta::info entry)
        {
            const auto definitions = definitionsOf(entry);
            if (definitions.size() != 1) {
                throw "Thread entries require exactly one Definition annotation";
            }
            return std::meta::extract<thread::Definition>(definitions.front());
        }

        bool waitForLaunch()
        {
            std::unique_lock lock(m_mutex);
            m_ready.wait(lock, [this] { return m_released; });
            return m_launch;
        }

        void release(bool launch)
        {
            {
                std::scoped_lock lock(m_mutex);
                m_launch = launch;
                m_released = true;
            }
            m_ready.notify_all();
        }

        bool m_started{};
        bool m_released{};
        bool m_launch{};
        std::mutex m_mutex;
        std::condition_variable m_ready;
        std::vector<std::thread> m_threads;
    };
}
