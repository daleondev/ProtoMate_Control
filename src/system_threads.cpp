#include "system_threads.hpp"

#include "runtime/thread.hpp"

#include <meta>
#include <mutex>
#include <string_view>

namespace system_threads
{
    namespace
    {
        using Prio = decltype(PRIO._0);

        consteval auto get_thread_functions()
        {
            std::vector<std::meta::info> functions;

            for (auto func : std::meta::members_of(^^system_threads, std::meta::access_context::current())) {
                if (std::meta::is_function(func) && func != ^^start) {
                    functions.push_back(func);
                }
            }

            return std::define_static_array(functions);
        }

        consteval auto validate_thread_functions() -> bool
        {
            constexpr auto thread_functions{ get_thread_functions() };

            for (auto func : thread_functions) {
                if (std::meta::annotations_of_with_type(func, ^^Prio).size() != 1) {
                    return false;
                }
                if (std::meta::annotations_of_with_type(func, ^^pnm::units::ByteSize).size() != 1) {
                    return false;
                }
            }
            return true;
        }

        static_assert(validate_thread_functions());

        auto start_all() -> void
        {
            static constexpr auto thread_functions{ get_thread_functions() };

            static std::vector<std::thread> threads;
            threads.reserve(thread_functions.size());

            template for (constexpr auto thread_function : thread_functions)
            {
                static_assert(thread_function != std::meta::current_function());

                constexpr auto priority{ [] -> int32_t {
                    constexpr auto annotations{ std::define_static_array(
                      std::meta::annotations_of_with_type(thread_function, ^^Prio)) };
                    return std::meta::extract<Prio>(annotations[0]);
                }() };

                constexpr auto stack_size{ [] -> size_t {
                    constexpr auto annotations{ std::define_static_array(
                      std::meta::annotations_of_with_type(thread_function, ^^pnm::units::ByteSize)) };
                    return std::meta::extract<pnm::units::ByteSize>(annotations[0])
                      .get<pnm::units::ByteSizeUnits::bytes>();
                }() };

                runtime::thread::Attributes attr{ .name = std::meta::identifier_of(thread_function),
                                                  .priority = priority,
                                                  .stack_size = stack_size };
                threads.push_back(runtime::thread::create(attr, [:thread_function:]));
            }
        }
    }

    auto start() -> void
    {
        static std::once_flag flag;
        std::call_once(flag, start_all);
    }
}