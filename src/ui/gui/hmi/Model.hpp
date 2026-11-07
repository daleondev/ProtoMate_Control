#pragma once
#include "Backend.hpp"
#include <cstdio>
#include <cstring>
namespace ui::gui::hmi
{
    enum class Page
    {
        Home,
        Axes,
        Robot,
        Cartesian,
        Preview,
        Motors,
        Programs,
        Presets,
        IO,
        Settings,
        Jobs,
        Alarms
    };
    struct Model
    {
        Backend* backend{};
        Snapshot state{};
        Page page{ Page::Home };
        unsigned axis{}, coordinates{}, jobOffset{}, buffer{};
        unsigned axisTab{}, motorTab{}, settingsTab{}, ioTab{}, diagnosticTab{}, diagnosticOffset{},
          driverDetail{};
        unsigned jogMode{};
        bool more{}, advanced{}, diagnosticIO{};
        bool feedback{ true }, continuous{}, absolute{}, previews{}, keypad{}, confirmation{};
        unsigned field{};
        std::array<double, 12> values{ 0, 0, 0, 0, 0, 0, 0, 0, 1, .5, .25, 10 };
        // position x/y/z; speed,a,d,j,timeout,duration,latch,factor,brightness.
        double step{ 1 };
        Text<32> number{};
        Text<192> notice{};
        std::array<std::uint32_t, 8> graphics{};
        std::uint64_t watchedJob{};
        unsigned watchedState{ ~0U };
        std::array<Job, 4> jobs{};
        unsigned jobCount{};
        std::array<Text<160>, 8> messages{};
        unsigned messageCount{};
        std::uint64_t lastOperation{};
        std::uint32_t ticks{}, interactions{};
        // Kept for CLI graphics diagnostic compatibility.
        unsigned value{ 42 };
        bool animating{};
        void report(const char* text)
        {
            if (std::strncmp(notice.data(), text, notice.size()) == 0)
                return;
            std::snprintf(notice.data(), notice.size(), "%s", text);
            auto& slot = messages[messageCount++ % messages.size()];
            std::snprintf(slot.data(), slot.size(), "%s", text);
        }
    };
}
