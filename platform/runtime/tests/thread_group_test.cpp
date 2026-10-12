#include "runtime/ThreadGroup.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>

namespace
{
    class Owner
    {
        friend class runtime::ThreadGroup;
        static constexpr runtime::thread::Definition FIRST{.priority = 10, .stack_size = 16U * 1024U};
        static constexpr runtime::thread::Definition SECOND{.priority = 11, .stack_size = 16U * 1024U};
        [[= FIRST]] void first()
        {
            auto* current = tx_thread_identify();
            first_priority = current->tx_thread_priority;
            first_stack = current->tx_thread_stack_size;
            first_name = current->tx_thread_name;
            ++calls;
        }
        [[= SECOND]] void second() { ++calls; }
        void ordinaryHelper() { calls += 100; }

      public:
        std::atomic<int> calls{};
        UINT first_priority{};
        ULONG first_stack{};
        std::string first_name;
        runtime::ThreadGroup threads;
    };

    TEST(ThreadGroup, OnlyAnnotatedInstanceMethodsRunWithTheirAttributes)
    {
        Owner first;
        Owner second;
        first.threads.start(first, "first");
        second.threads.start(second, "second");
        first.threads.join();
        second.threads.join();
        EXPECT_EQ(first.calls.load(), 2);
        EXPECT_EQ(second.calls.load(), 2);
        EXPECT_EQ(first.first_priority, 10U);
        // ThreadX reserves a guard word; the runtime formats diagnostic names.
        EXPECT_EQ(first.first_stack, 16U * 1024U - sizeof(ULONG));
        EXPECT_EQ(first.first_name, "First.first Thread");
        EXPECT_EQ(second.first_name, "Second.first Thread");
    }

    TEST(ThreadGroup, RepeatedJoinIsHarmlessButStartCannotRepeat)
    {
        Owner owner;
        owner.threads.join();
        owner.threads.start(owner, "test");
        owner.threads.join();
        owner.threads.join();
        EXPECT_THROW(owner.threads.start(owner, "test"), std::logic_error);
    }

    class FailingOwner
    {
        friend class runtime::ThreadGroup;
        static constexpr runtime::thread::Definition FIRST{.priority = 10, .stack_size = 16U * 1024U};
        // Rejected by the runtime's stack-size overflow validation, without allocating.
        static constexpr runtime::thread::Definition SECOND{
          .priority = 11, .stack_size = std::numeric_limits<std::size_t>::max()};
        [[= FIRST]] void first() { ++calls; }
        [[= SECOND]] void second() { ++calls; }

      public:
        std::atomic<int> calls{};
        runtime::ThreadGroup threads;
    };

    TEST(ThreadGroup, PartialCreationFailureJoinsThreadsWithoutRunningOwnerEntries)
    {
        FailingOwner owner;
        EXPECT_THROW(owner.threads.start(owner, "failure"), std::system_error);
        owner.threads.join();
        EXPECT_EQ(owner.calls.load(), 0);
    }
}
