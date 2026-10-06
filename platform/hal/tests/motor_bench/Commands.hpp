#pragma once

#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

namespace motor_bench
{
    enum class Operation { Help, Check, Status, Hold, Move, Forward, Reverse, Independent, Feedback, Stop };
    struct Command
    {
        Operation operation{ Operation::Help };
        unsigned axes{}; // Bit 0 = M1, bit 1 = M2.
        std::uint32_t pulses{ 400 }, hertz{ 200 };
    };

    constexpr auto name(Operation operation) -> const char*
    {
        switch (operation) {
            case Operation::Help: return "help";
            case Operation::Check: return "check";
            case Operation::Status: return "status";
            case Operation::Hold: return "hold";
            case Operation::Move: return "move";
            case Operation::Forward: return "forward";
            case Operation::Reverse: return "reverse";
            case Operation::Independent: return "independent";
            case Operation::Feedback: return "feedback";
            case Operation::Stop: return "stop";
        }
        return "unknown";
    }

    // Reject the entire line on malformed, extra or out-of-range arguments.
    // All user-selected moves are finite, at most one motor revolution / leg,
    // 50..1000 Hz and at most 20 seconds per leg (assuming 1/16 microstepping).
    inline auto parse(std::string_view line) -> std::optional<Command>
    {
        std::array<std::string_view, 5> words{};
        unsigned size{};
        while (!line.empty()) {
            const auto first = line.find_first_not_of(" \t\r\n");
            if (first == line.npos) break;
            line.remove_prefix(first);
            const auto end = line.find_first_of(" \t\r\n");
            if (size == words.size()) return {};
            words[size++] = line.substr(0, end);
            if (end == line.npos) break;
            line.remove_prefix(end);
        }
        Command result;
        if (size == 0) return result;
        const auto name = words[0];
        if (name == "help") result.operation = Operation::Help;
        else if (name == "check") result.operation = Operation::Check;
        else if (name == "status") result.operation = Operation::Status;
        else if (name == "hold") result.operation = Operation::Hold;
        else if (name == "stop") result.operation = Operation::Stop;
        else if (name == "independent") result.operation = Operation::Independent;
        else if (name == "move") result.operation = Operation::Move;
        else if (name == "forward") result.operation = Operation::Forward;
        else if (name == "reverse") result.operation = Operation::Reverse;
        else if (name == "feedback") result.operation = Operation::Feedback;
        else return {};
        const bool movement = result.operation == Operation::Move ||
                              result.operation == Operation::Forward || result.operation == Operation::Reverse;
        if (!movement && result.operation != Operation::Feedback) {
            if (size != 1) return {};
            return result;
        }
        if (size < 2 || size > (movement ? 4U : 2U)) return {};
        if (words[1] == "m1") result.axes = 1;
        else if (words[1] == "m2") result.axes = 2;
        else if (words[1] == "both") result.axes = 3;
        else return {};
        for (unsigned i = 2; i < size; ++i) {
            auto& value = i == 2 ? result.pulses : result.hertz;
            const auto [end, error] = std::from_chars(words[i].data(), words[i].data() + words[i].size(), value);
            if (error != std::errc{} || end != words[i].data() + words[i].size()) return {};
        }
        if (result.pulses == 0 || result.pulses > 3200 || result.hertz < 50 || result.hertz > 1000 ||
            result.pulses > result.hertz * 20U) return {};
        return result;
    }
}
