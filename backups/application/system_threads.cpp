#include "system_threads.hpp"

#include "runtime/thread.hpp"

#include <algorithm>
#include <meta>
#include <mutex>
#include <string_view>

namespace system_threads
{
    namespace
    {
        using Prio = decltype(PRIO._0);

        // Reflection represents both entities and types as info; their roles are explicit at call sites.
        // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
        consteval auto annotations_of_type(std::meta::info entity, std::meta::info type)
        {
            std::vector<std::meta::info> matches;
            for (const auto annotation : std::meta::annotations_of(entity)) {
                if (std::meta::is_same_type(std::meta::remove_const(std::meta::type_of(annotation)),
                                            std::meta::remove_const(type))) {
                    matches.push_back(annotation);
                }
            }
            return matches;
        }

        consteval auto get_thread_functions()
        {
            std::vector<std::meta::info> functions;
            constexpr auto entry_name{ std::meta::identifier_of(^^start) };

            for (auto func : std::meta::members_of(^^system_threads, std::meta::access_context::current())) {
                // A prototype compiler can give declarations and definitions
                // distinct reflection handles. Exclude the named entry point.
                if (std::meta::is_function(func) && std::meta::identifier_of(func) != entry_name) {
                    functions.push_back(func);
                }
            }

            return std::define_static_array(functions);
        }

        consteval auto validate_thread_functions() -> bool
        {
            constexpr auto thread_functions{ get_thread_functions() };

            return std::ranges::all_of(thread_functions, [](auto func) {
                return annotations_of_type(func, ^^Prio).size() == 1 &&
                       annotations_of_type(func, ^^pnm::units::ByteSize).size() == 1;
            });
        }

        static_assert(validate_thread_functions());

        auto start_all() -> void
        {
            static constexpr auto thread_functions{ get_thread_functions() };
            constexpr auto thread_count{ thread_functions.size() };

            static std::vector<std::thread> threads;
            threads.reserve(thread_count);

            // Clang synthesizes reserved __N/__range identifiers for expansion statements.
            // NOLINTBEGIN(bugprone-reserved-identifier,readability-identifier-naming)
            template for (constexpr auto thread_function : thread_functions)
            {
                // NOLINTEND(bugprone-reserved-identifier,readability-identifier-naming)
                static_assert(std::meta::identifier_of(thread_function) != "start_all");

                constexpr auto priority{ [] -> int32_t {
                    constexpr auto annotations{ std::define_static_array(
                      annotations_of_type(thread_function, ^^Prio)) };
                    return std::meta::extract<Prio>(annotations[0]);
                }() };

                constexpr auto stack_size{ [] -> size_t {
                    constexpr auto annotations{ std::define_static_array(
                      annotations_of_type(thread_function, ^^pnm::units::ByteSize)) };
                    return std::meta::extract<pnm::units::ByteSize>(annotations[0])
                      .template get<pnm::units::ByteSizeUnits::bytes>();
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
