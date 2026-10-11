#include "Frame.hpp"

#include <utility>

namespace p2p::util
{
    namespace
    {
        void write_integer(std::span<std::byte> bytes, std::uint64_t value)
        {
            for (auto& byte : bytes) {
                byte = static_cast<std::byte>(value & 0xFFU);
                value >>= 8U;
            }
        }

        std::uint64_t read_integer(std::span<const std::byte> bytes)
        {
            std::uint64_t value{};
            for (std::size_t i = 0; i < bytes.size(); ++i)
                value |= std::to_integer<std::uint64_t>(bytes[i]) << (8U * i);
            return value;
        }
    }

    OutgoingFrame makeFrame(FrameKind kind, const std::string& name,
                            std::uint64_t id, std::size_t payloadSize)
    {
        const auto nameSize = pnm::utils::memory::SerializationAdapter<std::string>::bufferSize(name);
        if (nameSize > MAX_FRAME_BYTES - HEADER_BYTES ||
            payloadSize > MAX_FRAME_BYTES - HEADER_BYTES - nameSize)
            throw std::length_error("Serialized message exceeds bridge frame capacity");
        OutgoingFrame frame;
        frame.size = HEADER_BYTES + nameSize + payloadSize;
        frame.bytes[0] = static_cast<std::byte>(FRAME_VERSION);
        frame.bytes[1] = static_cast<std::byte>(kind);
        write_integer(std::span(frame.bytes).subspan(2, 2), nameSize);
        write_integer(std::span(frame.bytes).subspan(4, 8), id);
        if (!pnm::utils::memory::serialize(name, std::span(frame.bytes).subspan(HEADER_BYTES, nameSize)))
            throw std::length_error("Cannot serialize bridge endpoint name");
        return frame;
    }

    IncomingFrame decodeFrame(std::span<const std::byte> frame)
    {
        if (frame.size() < HEADER_BYTES || frame.size() > MAX_FRAME_BYTES)
            throw std::invalid_argument("Invalid bridge frame size");
        if (std::to_integer<std::uint8_t>(frame[0]) != FRAME_VERSION)
            throw std::invalid_argument("Unsupported bridge frame version");
        const auto kind = static_cast<FrameKind>(frame[1]);
        const auto nameSize = read_integer(frame.subspan(2, 2));
        const auto id = read_integer(frame.subspan(4, 8));
        const auto budget = read_integer(frame.subspan(12, 4));
        if (nameSize > frame.size() - HEADER_BYTES)
            throw std::invalid_argument("Invalid bridge name length");
        std::string name;
        if (!pnm::utils::memory::deserialize(frame.subspan(HEADER_BYTES, nameSize), name))
            throw std::invalid_argument("Invalid bridge name");
        const auto payload = frame.subspan(HEADER_BYTES + nameSize);
        return { kind, std::move(name), id, static_cast<std::uint32_t>(budget), payload };
    }

    void setRequestBudget(OutgoingFrame& frame, std::uint32_t milliseconds)
    {
        write_integer(std::span(frame.bytes).subspan(12, 4), milliseconds);
    }
}
