#include "Frame.hpp"

#include <limits>
#include <utility>

namespace icc::util
{
    namespace
    {
        constexpr unsigned BYTE_BITS{ 8 };
        constexpr std::uint64_t BYTE_MASK{ std::numeric_limits<std::uint8_t>::max() };
        constexpr std::size_t NAME_LENGTH_OFFSET{ 2 };
        constexpr std::size_t ID_OFFSET{ 4 };
        constexpr std::size_t BUDGET_OFFSET{ 12 };
        void write_integer(std::span<std::byte> bytes, std::uint64_t value)
        {
            for (auto& byte : bytes) {
                byte = static_cast<std::byte>(value & BYTE_MASK);
                value >>= BYTE_BITS;
            }
        }

        std::uint64_t read_integer(std::span<const std::byte> bytes)
        {
            std::uint64_t value{};
            for (std::size_t i = 0; i < bytes.size(); ++i) {
                value |= std::to_integer<std::uint64_t>(bytes[i]) << (BYTE_BITS * i);
            }
            return value;
        }
    }

    // ID and byte count have distinct protocol roles; callers use named values.
    // NOLINTBEGIN(bugprone-easily-swappable-parameters)
    OutgoingFrame make_frame(FrameKind kind,
                             const std::string& name,
                             std::uint64_t id,
                             std::size_t payload_size)
    {
        // NOLINTEND(bugprone-easily-swappable-parameters)
        const auto name_size = pnm::utils::memory::SerializationAdapter<std::string>::bufferSize(name);
        if (name_size > MAX_FRAME_BYTES - HEADER_BYTES ||
            payload_size > MAX_FRAME_BYTES - HEADER_BYTES - name_size) {
            throw std::length_error("Serialized message exceeds bridge frame capacity");
        }
        OutgoingFrame frame;
        frame.size = HEADER_BYTES + name_size + payload_size;
        frame.bytes[0] = static_cast<std::byte>(FRAME_VERSION);
        frame.bytes[1] = static_cast<std::byte>(kind);
        write_integer(std::span(frame.bytes).subspan(NAME_LENGTH_OFFSET, sizeof(std::uint16_t)), name_size);
        write_integer(std::span(frame.bytes).subspan(ID_OFFSET, sizeof(std::uint64_t)), id);
        if (!pnm::utils::memory::serialize(name, std::span(frame.bytes).subspan(HEADER_BYTES, name_size))) {
            throw std::length_error("Cannot serialize bridge endpoint name");
        }
        return frame;
    }

    IncomingFrame decode_frame(std::span<const std::byte> frame)
    {
        if (frame.size() < HEADER_BYTES || frame.size() > MAX_FRAME_BYTES) {
            throw std::invalid_argument("Invalid bridge frame size");
        }
        if (std::to_integer<std::uint8_t>(frame[0]) != FRAME_VERSION) {
            throw std::invalid_argument("Unsupported bridge frame version");
        }
        const auto kind = static_cast<FrameKind>(frame[1]);
        const auto name_size = read_integer(frame.subspan(NAME_LENGTH_OFFSET, sizeof(std::uint16_t)));
        const auto id = read_integer(frame.subspan(ID_OFFSET, sizeof(std::uint64_t)));
        const auto budget = read_integer(frame.subspan(BUDGET_OFFSET, sizeof(std::uint32_t)));
        if (name_size > frame.size() - HEADER_BYTES) {
            throw std::invalid_argument("Invalid bridge name length");
        }
        std::string name;
        if (!pnm::utils::memory::deserialize(frame.subspan(HEADER_BYTES, name_size), name)) {
            throw std::invalid_argument("Invalid bridge name");
        }
        const auto payload = frame.subspan(HEADER_BYTES + name_size);
        return { .kind = kind,
                 .name = std::move(name),
                 .id = id,
                 .budget = static_cast<std::uint32_t>(budget),
                 .payload = payload };
    }

    void set_request_budget(OutgoingFrame& frame, std::uint32_t milliseconds)
    {
        write_integer(std::span(frame.bytes).subspan(BUDGET_OFFSET, sizeof(std::uint32_t)), milliseconds);
    }
}
