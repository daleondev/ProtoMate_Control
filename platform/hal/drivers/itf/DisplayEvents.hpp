#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace hal::display
{
    enum class Event
    {
        VSync,
        Presented,
        Error,
        Close
    };
    struct Callback
    {
        void (*function)(void*, Event) noexcept {};
        void* context{};
        void operator()(Event event) const noexcept
        {
            if (function) {
                function(context, event);
            }
        }
    };
    struct State
    {
        unsigned active{};
        std::optional<unsigned> pending;
        std::uint32_t vsyncs{}, presentations{}, underruns{}, errors{};
        bool closed{};
    };
}
