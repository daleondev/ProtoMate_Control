#pragma once

#include "libstdcxx/backend.hpp"

#include <concepts>
#include <functional>
#include <stop_token>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>

namespace runtime::thread
{
    namespace detail
    {
        class PendingAttributesScope
        {
          public:
            explicit PendingAttributesScope(const Attributes& attributes) noexcept
              : m_previous_attributes{ consume_attributes() }
            {
                publish_attributes(attributes);
            }

            PendingAttributesScope(const PendingAttributesScope&) = delete;
            PendingAttributesScope(PendingAttributesScope&&) = delete;
            auto operator=(const PendingAttributesScope&) -> PendingAttributesScope& = delete;
            auto operator=(PendingAttributesScope&&) -> PendingAttributesScope& = delete;

            ~PendingAttributesScope()
            {
                // Clear attributes that were not consumed because construction
                // failed, then restore an enclosing/manual publication.
                static_cast<void>(consume_attributes());
                if (m_previous_attributes.has_value()) {
                    publish_attributes(*m_previous_attributes);
                }
            }

          private:
            std::optional<Attributes> m_previous_attributes;
        };

        template<typename Callable, typename... Arguments>
        class Invocation
        {
          public:
            template<typename SourceCallable, typename... SourceArguments>
            explicit Invocation(SourceCallable&& callable, SourceArguments&&... arguments)
              : m_callable{ std::forward<SourceCallable>(callable) }
              , m_arguments{ std::forward<SourceArguments>(arguments)... }
            {
            }

            void operator()() noexcept(std::is_nothrow_invocable_v<Callable, Arguments...>)
                requires std::is_invocable_v<Callable, Arguments...>
            {
                std::apply([this](auto&... arguments) {
                    static_cast<void>(std::invoke(std::move(m_callable), std::move(arguments)...));
                }, m_arguments);
            }

            void operator()(std::stop_token token) noexcept(
              std::is_nothrow_invocable_v<Callable, std::stop_token, Arguments...>)
                requires std::is_invocable_v<Callable, std::stop_token, Arguments...>
            {
                std::apply([this, token = std::move(token)](auto&... arguments) mutable {
                    static_cast<void>(
                      std::invoke(std::move(m_callable), std::move(token), std::move(arguments)...));
                }, m_arguments);
            }

          private:
            Callable m_callable;
            std::tuple<Arguments...> m_arguments;
        };

        template<typename Callable, typename... Arguments>
        concept DecayCopyableInvocation =
          std::constructible_from<std::decay_t<Callable>, Callable> &&
          (std::constructible_from<std::decay_t<Arguments>, Arguments> && ...);

        template<typename Callable, typename... Arguments>
        concept Invocable = std::is_invocable_v<std::decay_t<Callable>, std::decay_t<Arguments>...>;

        template<typename Callable, typename... Arguments>
        concept StopTokenInvocable =
          std::is_invocable_v<std::decay_t<Callable>, std::stop_token, std::decay_t<Arguments>...>;

        template<typename Callable, typename... Arguments>
        using DecayedInvocation = Invocation<std::decay_t<Callable>, std::decay_t<Arguments>...>;

        template<typename Callable, typename... Arguments>
        [[nodiscard]] auto prepare_task(Callable&& callable, Arguments&&... arguments)
        {
            using PreparedInvocation = DecayedInvocation<Callable, Arguments...>;
            return std::move_only_function<void()>{ std::in_place_type<PreparedInvocation>,
                                                    std::forward<Callable>(callable),
                                                    std::forward<Arguments>(arguments)... };
        }

        template<typename Callable, typename... Arguments>
        [[nodiscard]] auto prepare_stoppable_task(Callable&& callable, Arguments&&... arguments)
        {
            using PreparedInvocation = DecayedInvocation<Callable, Arguments...>;
            return std::move_only_function<void(std::stop_token)>{ std::in_place_type<PreparedInvocation>,
                                                                   std::forward<Callable>(callable),
                                                                   std::forward<Arguments>(arguments)... };
        }
    }

    // The invocation is completely decay-copied before attributes are
    // published. It is then transferred through a noexcept type-erased move so
    // callable or argument move operations cannot run in the
    // publication-to-consumption window.
    template<typename Callable, typename... Arguments>
        requires detail::DecayCopyableInvocation<Callable, Arguments...> &&
                 detail::Invocable<Callable, Arguments...>
    [[nodiscard]] auto create(const Attributes& attributes, Callable&& callable, Arguments&&... arguments)
      -> std::thread
    {
        auto task{ detail::prepare_task(std::forward<Callable>(callable),
                                        std::forward<Arguments>(arguments)...) };

        const detail::PendingAttributesScope pending_attributes{ attributes };
        return std::thread{ std::move(task) };
    }

    // Matches std::jthread semantics: a stop token is supplied whenever the
    // decay-copied callable accepts one as its first argument.
    template<typename Callable, typename... Arguments>
        requires detail::DecayCopyableInvocation<Callable, Arguments...> &&
                 (detail::StopTokenInvocable<Callable, Arguments...> ||
                  detail::Invocable<Callable, Arguments...>)
    [[nodiscard]] auto create_jthread(const Attributes& attributes,
                                      Callable&& callable,
                                      Arguments&&... arguments) -> std::jthread
    {
        if constexpr (detail::StopTokenInvocable<Callable, Arguments...>) {
            auto task{ detail::prepare_stoppable_task(std::forward<Callable>(callable),
                                                      std::forward<Arguments>(arguments)...) };
            const detail::PendingAttributesScope pending_attributes{ attributes };
            return std::jthread{ std::move(task) };
        }
        else {
            auto task{ detail::prepare_task(std::forward<Callable>(callable),
                                            std::forward<Arguments>(arguments)...) };
            const detail::PendingAttributesScope pending_attributes{ attributes };
            return std::jthread{ std::move(task) };
        }
    }
}
