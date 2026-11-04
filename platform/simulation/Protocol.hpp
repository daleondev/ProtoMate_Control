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
    inline constexpr std::uint32_t VERSION{ 1 };
    inline constexpr std::size_t FRAME_BYTES{ 2048 }, PAYLOAD_BYTES{ 1536 };
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
        PwmStop
    };
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
        std::array<std::uint64_t, 8> arguments{};
        std::array<std::byte, PAYLOAD_BYTES> data{};
    };
    inline constexpr std::size_t PACKET_HEADER_BYTES{ 104 };
    inline constexpr std::size_t PACKET_BYTES{ PACKET_HEADER_BYTES + PAYLOAD_BYTES };
    static_assert(PACKET_BYTES + 64 < FRAME_BYTES);

    inline void put(std::span<std::byte> out, std::size_t offset, std::uint64_t value, unsigned bytes)
    {
        if (bytes > 8 || offset > out.size() || bytes > out.size() - offset)
            throw std::out_of_range{ "simulation encode" };
        for (unsigned i{}; i < bytes; ++i)
            out[offset + i] = std::byte(value >> (8U * i));
    }
    inline std::uint64_t get(std::span<const std::byte> in, std::size_t offset, unsigned bytes)
    {
        if (bytes > 8 || offset > in.size() || bytes > in.size() - offset)
            throw std::out_of_range{ "simulation decode" };
        std::uint64_t value{};
        for (unsigned i{}; i < bytes; ++i)
            value |= std::uint64_t(std::to_integer<unsigned>(in[offset + i])) << (8U * i);
        return value;
    }
    inline auto encode(const Packet& packet) -> std::array<std::byte, PACKET_BYTES>
    {
        if (packet.size > PAYLOAD_BYTES)
            throw std::length_error{ "simulation payload" };
        std::array<std::byte, PACKET_BYTES> out{};
        put(out, 0, VERSION, 4);
        put(out, 4, std::to_underlying(packet.operation), 4);
        put(out, 8, packet.status, 4);
        put(out, 12, packet.size, 4);
        put(out, 16, packet.sequence, 8);
        put(out, 24, packet.session, 8);
        put(out, 32, packet.time_ns, 8);
        for (unsigned i{}; i < 8; ++i)
            put(out, 40 + i * 8, packet.arguments[i], 8);
        for (std::size_t i{}; i < packet.size; ++i)
            out[PACKET_HEADER_BYTES + i] = packet.data[i];
        return out;
    }
    inline Packet decode(std::span<const std::byte> in)
    {
        if (in.size() != PACKET_BYTES || get(in, 0, 4) != VERSION)
            throw std::invalid_argument{ "simulation protocol version/length" };
        Packet packet{};
        const auto op{ get(in, 4, 4) };
        if (op > std::to_underlying(Operation::PwmStop))
            throw std::invalid_argument{ "simulation operation" };
        packet.operation = static_cast<Operation>(op);
        packet.status = static_cast<std::uint32_t>(get(in, 8, 4));
        packet.size = static_cast<std::uint32_t>(get(in, 12, 4));
        if (packet.size > PAYLOAD_BYTES)
            throw std::length_error{ "simulation payload" };
        packet.sequence = get(in, 16, 8);
        packet.session = get(in, 24, 8);
        packet.time_ns = get(in, 32, 8);
        for (unsigned i{}; i < 8; ++i)
            packet.arguments[i] = get(in, 40 + i * 8, 8);
        for (std::size_t i{}; i < packet.size; ++i)
            packet.data[i] = in[PACKET_HEADER_BYTES + i];
        return packet;
    }
    inline std::uint32_t checksum(std::span<const std::byte> data) noexcept
    {
        std::uint32_t hash{ 2166136261U };
        for (auto b : data)
            hash = (hash ^ std::to_integer<std::uint32_t>(b)) * 16777619U;
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
