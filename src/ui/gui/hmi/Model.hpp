#pragma once
#include "Backend.hpp"
#include "Text.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
namespace ui::gui::hmi
{
    enum class Page : std::uint8_t
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
    namespace fields
    {
        inline constexpr unsigned POSITION_1{ 0 };
        inline constexpr unsigned POSITION_2{ 1 };
        inline constexpr unsigned POSITION_3{ 2 };
        inline constexpr unsigned SPEED{ 3 };
        inline constexpr unsigned ACCELERATION{ 4 };
        inline constexpr unsigned DECELERATION{ 5 };
        inline constexpr unsigned JERK{ 6 };
        inline constexpr unsigned TIMEOUT{ 7 };
        inline constexpr unsigned DURATION{ 8 };
        inline constexpr unsigned LATCH_SPEED{ 9 };
        inline constexpr unsigned SPEED_FACTOR{ 10 };
        inline constexpr unsigned BRIGHTNESS{ 11 };
        inline constexpr std::array<double, 12> DEFAULTS{ 0, 0, 0, 0, 0, 0, 0, 0, 1, .5, .25, 10 };
    }
    inline constexpr unsigned MESSAGE_HISTORY_CAPACITY{ 8 };
    struct Model
    {
        Backend* backend{};
        Snapshot state{};
        Page page{ Page::Home };
        unsigned axis{}, coordinates{}, job_offset{}, buffer{};
        unsigned axis_tab{}, motor_tab{}, settings_tab{}, io_tab{}, diagnostic_tab{}, diagnostic_offset{},
          driver_detail{};
        unsigned jog_mode{ 2 }, cartesian_jog_mode{ 2 }, cartesian_tab{};
        bool more{}, advanced{}, diagnostic_io{};
        bool absolute{}, previews{}, keypad{}, confirmation{}, show_info{ true };
        unsigned field{};
        std::uint32_t editor_revision{};
        std::array<double, fields::DEFAULTS.size()> values{ fields::DEFAULTS };
        // position x/y/z; speed,a,d,j,timeout,duration,latch,factor,brightness.
        double step{ 1 };
        double cartesian_step{ 1 };
        Text<text_capacity::SHORT> number{};
        Text<text_capacity::DETAIL> notice{};
        std::array<std::uint32_t, GRAPHICS_METRICS> graphics{};
        std::uint64_t watched_job{};
        unsigned watched_state{ ~0U };
        std::array<Job, 4> jobs{};
        unsigned job_count{};
        struct Message
        {
            Text<text_capacity::MESSAGE> text{};
            Severity severity{ Severity::Info };
        };
        std::array<Message, MESSAGE_HISTORY_CAPACITY> messages{};
        Severity notice_severity{ Severity::Info };
        unsigned message_count{};
        std::uint64_t last_operation{};
        std::uint32_t ticks{}, interactions{};
        // Kept for CLI graphics diagnostic compatibility.
        unsigned value{ INITIAL_GRAPHICS_VALUE };
        bool animating{};
        void report(const char* text, Severity severity = Severity::Info)
        {
            if (!*text ||
                (notice_severity == severity && std::strncmp(notice.data(), text, notice.size()) == 0)) {
                return;
            }
            format(notice, "{}", text);
            notice_severity = severity;
            auto& slot{ messages[message_count++ % messages.size()] };
            format(slot.text, "{}", text);
            slot.severity = severity;
        }
        const Message* visibleMessage(unsigned index) const
        {
            for (unsigned i{ 0 }; i < std::min(message_count, unsigned(messages.size())); ++i) {
                const auto& message{ messages[(message_count - 1 - i) % messages.size()] };
                if (!show_info && message.severity == Severity::Info) {
                    continue;
                }
                if (index-- == 0) {
                    return &message;
                }
            }
            return nullptr;
        }
    };
}
