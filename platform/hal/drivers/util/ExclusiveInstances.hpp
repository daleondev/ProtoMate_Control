#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <utility>

namespace hal::util
{
    // A claim outlives the concrete driver's destructor. weak_ptr::expired()
    // alone is insufficient: it becomes true before hardware deinitialization.
    // The pool must outlive every instance; factories keep their pool static.
    template<std::size_t Count>
    class ExclusiveInstances final
    {
        class Lease
        {
          public:
            explicit Lease(std::atomic_bool& claimed)
              : m_claimed{ &claimed }
            {
            }
            Lease(Lease&& other) noexcept
              : m_claimed{ std::exchange(other.m_claimed, nullptr) }
            {
            }
            ~Lease()
            {
                if (m_claimed)
                    m_claimed->store(false, std::memory_order_release);
            }
            Lease(const Lease&) = delete;
            Lease& operator=(const Lease&) = delete;

          private:
            std::atomic_bool* m_claimed;
        };

        template<class Driver>
        struct Instance
        {
            template<class... Args>
            Instance(Lease claim, Args&&... args)
              : lease{ std::move(claim) }
              , driver{ std::forward<Args>(args)... }
            {
            }
            Lease lease; // Destroyed after driver, including failed construction.
            Driver driver;
        };

      public:
        template<class Driver, class... Args>
        [[nodiscard]] auto create(std::size_t index, Args&&... args) -> std::shared_ptr<Driver>
        {
            if (index >= Count)
                return {};
            bool expected{};
            if (!m_claimed[index].compare_exchange_strong(expected, true, std::memory_order_acq_rel))
                return {};
            Lease lease{ m_claimed[index] };
            auto instance{ std::make_shared<Instance<Driver>>(std::move(lease),
                                                              std::forward<Args>(args)...) };
            auto* driver{ &instance->driver };
            return { std::move(instance), driver };
        }

      private:
        std::array<std::atomic_bool, Count> m_claimed{};
    };
}
