#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>

namespace simulation
{
    inline constexpr std::uint32_t VERSION{ 4 };
    inline constexpr std::size_t FRAME_BYTES{ 2048 }, PAYLOAD_BYTES{ 1536 };
    // Ratios, lead, native origins/directions, geometry, cam contact datums and separate usable soft
    // endpoints.
    namespace mechanical
    {
        inline constexpr std::size_t RATIO{ 0 }, LEAD{ 3 }, AXIS_REFERENCE{ 4 }, SHAFT_REFERENCE{ 7 },
          DIRECTION{ 10 }, GEOMETRY{ 13 }, TOOL_Z{ 16 }, UPPER_CONTACT{ 17 }, LOWER_CONTACT{ 20 },
          SOFT_MINIMUM{ 23 }, SOFT_MAXIMUM{ 26 }, FIELDS{ 29 };
    }
    namespace state_wire
    {
        inline constexpr std::size_t VALUE_BYTES{ sizeof(std::uint64_t) }, AXES{ 3 }, SHAFT{ 0 },
          JOINT{ AXES * VALUE_BYTES }, TCP{ 2 * AXES * VALUE_BYTES }, REJECTED{ 3 * AXES * VALUE_BYTES },
          BYTES{ 4 * AXES * VALUE_BYTES };
    }
    inline constexpr std::size_t ARGUMENT_COUNT{ 8 }, DESCRIPTOR_COUNT{ 5 };
    namespace step_wire
    {
        inline constexpr unsigned HEADER_WORDS{ 3 }, CHANNEL_WORDS{ 8 }, PENDING_FIELD{ 5 }, HIGH_FIELD{ 6 };
    }
    using MechanicalConfiguration = std::array<double, mechanical::FIELDS>;
    // Fixed representation is part of the existing protocol/diagnostic contract.
    // NOLINTNEXTLINE(performance-enum-size)
    enum class Operation : std::uint32_t
    {
        Hello,
        Begin,
        End,
        Reset,
        Arm,
        Start,
        Guard,
        Watch,
        Sample,
        Acknowledge,
        FinishAxis,
        StopAxis,
        Stop,
        Publish,
        GpioConfigure,
        GpioRead,
        GpioWrite,
        EncoderRun,
        EncoderRead,
        EncoderReset,
        Uart,
        Fault,
        Power,
        State,
        Shutdown,
        MediaInfo,
        MediaFlush,
        MediaRead,
        MediaWrite,
        MediaErase,
        TimerConfigure,
        TimerRead,
        TimerStart,
        TimerStop,
        PwmConfigure,
        PwmStart,
        PwmStop,
        Retarget,
        PublishFuture,
        Configuration
    };
    // Fixed representation is part of the existing protocol/diagnostic contract.
    // NOLINTNEXTLINE(performance-enum-size)
    enum class Fault : std::uint32_t
    {
        None,
        DriverAlarm,
        ReferenceStuck,
        ReferenceDisconnected,
        EncoderDisconnected,
        LostStep,
        ShaftSlip,
        DmaDelay,
        InterruptDelay,
        UartDisconnected,
        UartCorrupt
    };
    struct Packet
    {
        Operation operation{};
        std::uint32_t status{}, size{};
        std::uint64_t sequence{}, session{}, time_ns{};
        std::array<std::uint64_t, ARGUMENT_COUNT> arguments{};
        std::array<std::byte, PAYLOAD_BYTES> data{};
    };
    inline constexpr std::size_t PACKET_HEADER_BYTES{ 104 };
    inline constexpr std::size_t PACKET_BYTES{ PACKET_HEADER_BYTES + PAYLOAD_BYTES };
    inline constexpr std::size_t FRAME_OVERHEAD_RESERVE{ 64 };
    static_assert(PACKET_BYTES + FRAME_OVERHEAD_RESERVE < FRAME_BYTES);

    namespace wire
    {
        inline constexpr unsigned BYTE_BITS{ 8 }, U32_BYTES{ 4 }, U64_BYTES{ 8 };
        inline constexpr std::size_t VERSION_OFFSET{ 0 }, OPERATION_OFFSET{ 4 }, STATUS_OFFSET{ 8 },
          SIZE_OFFSET{ 12 }, SEQUENCE_OFFSET{ 16 }, SESSION_OFFSET{ 24 }, TIME_OFFSET{ 32 },
          ARGUMENTS_OFFSET{ 40 };
    }
    // Offset/value/width follows the binary field API; all callers specify the layout explicitly.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    inline void put(std::span<std::byte> out, std::size_t offset, std::uint64_t value, unsigned bytes)
    {
        if (bytes > wire::U64_BYTES || offset > out.size() || bytes > out.size() - offset) {
            throw std::out_of_range{ "simulation encode" };
        }
        for (unsigned i{}; i < bytes; ++i) {
            out[offset + i] = std::byte(value >> (wire::BYTE_BITS * i));
        }
    }
    inline std::uint64_t get(std::span<const std::byte> in, std::size_t offset, unsigned bytes)
    {
        if (bytes > wire::U64_BYTES || offset > in.size() || bytes > in.size() - offset) {
            throw std::out_of_range{ "simulation decode" };
        }
        std::uint64_t value{};
        for (unsigned i{}; i < bytes; ++i) {
            value |= std::uint64_t(std::to_integer<unsigned>(in[offset + i])) << (wire::BYTE_BITS * i);
        }
        return value;
    }
    inline auto encode(const Packet& packet) -> std::array<std::byte, PACKET_BYTES>
    {
        if (packet.size > PAYLOAD_BYTES) {
            throw std::length_error{ "simulation payload" };
        }
        std::array<std::byte, PACKET_BYTES> out{};
        put(out, wire::VERSION_OFFSET, VERSION, wire::U32_BYTES);
        put(out, wire::OPERATION_OFFSET, std::to_underlying(packet.operation), wire::U32_BYTES);
        put(out, wire::STATUS_OFFSET, packet.status, wire::U32_BYTES);
        put(out, wire::SIZE_OFFSET, packet.size, wire::U32_BYTES);
        put(out, wire::SEQUENCE_OFFSET, packet.sequence, wire::U64_BYTES);
        put(out, wire::SESSION_OFFSET, packet.session, wire::U64_BYTES);
        put(out, wire::TIME_OFFSET, packet.time_ns, wire::U64_BYTES);
        for (unsigned i{}; i < packet.arguments.size(); ++i) {
            put(out,
                wire::ARGUMENTS_OFFSET + (std::size_t(i) * wire::U64_BYTES),
                packet.arguments[i],
                wire::U64_BYTES);
        }
        for (std::size_t i{}; i < packet.size; ++i) {
            out[PACKET_HEADER_BYTES + i] = packet.data[i];
        }
        return out;
    }
    inline Packet decode(std::span<const std::byte> in)
    {
        if (in.size() != PACKET_BYTES || get(in, wire::VERSION_OFFSET, wire::U32_BYTES) != VERSION) {
            throw std::invalid_argument{ "simulation protocol version/length" };
        }
        Packet packet{};
        const auto op{ get(in, wire::OPERATION_OFFSET, wire::U32_BYTES) };
        if (op > std::to_underlying(Operation::Configuration)) {
            throw std::invalid_argument{ "simulation operation" };
        }
        packet.operation = static_cast<Operation>(op);
        packet.status = static_cast<std::uint32_t>(get(in, wire::STATUS_OFFSET, wire::U32_BYTES));
        packet.size = static_cast<std::uint32_t>(get(in, wire::SIZE_OFFSET, wire::U32_BYTES));
        if (packet.size > PAYLOAD_BYTES) {
            throw std::length_error{ "simulation payload" };
        }
        packet.sequence = get(in, wire::SEQUENCE_OFFSET, wire::U64_BYTES);
        packet.session = get(in, wire::SESSION_OFFSET, wire::U64_BYTES);
        packet.time_ns = get(in, wire::TIME_OFFSET, wire::U64_BYTES);
        for (unsigned i{}; i < packet.arguments.size(); ++i) {
            packet.arguments[i] =
              get(in, wire::ARGUMENTS_OFFSET + (std::size_t(i) * wire::U64_BYTES), wire::U64_BYTES);
        }
        for (std::size_t i{}; i < packet.size; ++i) {
            packet.data[i] = in[PACKET_HEADER_BYTES + i];
        }
        return packet;
    }
    inline std::uint32_t checksum(std::span<const std::byte> data) noexcept
    {
        constexpr std::uint32_t fnv_offset_basis{ 2166136261U };
        constexpr std::uint32_t fnv_prime{ 16777619U };
        std::uint32_t hash{ fnv_offset_basis };
        for (auto b : data) {
            hash = (hash ^ std::to_integer<std::uint32_t>(b)) * fnv_prime;
        }
        return hash;
    }
    struct PlantSnapshot
    {
        std::uint64_t time_ns{}, sequence{}, session{}, published_host_ns{};
        std::array<double, 3> shaft_degrees{}, joints{}, tcp_mm{};
        std::array<std::int64_t, 3> accepted_steps{};
        std::uint32_t references{}, faults{};
        bool powered{ true }, enabled{}, connected{};
    };
}
