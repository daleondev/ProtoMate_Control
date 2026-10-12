#pragma once

#include <pneumo/common.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>

namespace icc::util
{
    // v6: version u8, kind u8, name length u16, call ID u64, timeout ms u32,
    // name bytes, payload. All integer fields are little endian.
    // Service Cancel uses the urgent lane with a one-byte target lane (0/1).
    // ActionCancel/ActionAck use urgent; other action frames and topics use normal.
    // Hello/Ready/Goodbye are empty urgent frames with ID zero. Heartbeat/ACK
    // are empty urgent frames with their nonzero challenge ID.
    enum class FrameKind : std::uint8_t
    {
        Topic = 1,
        Request,
        Response,
        Error,
        Cancel,
        ActionGoal,
        ActionAccepted,
        ActionFeedback,
        ActionResult,
        ActionError,
        ActionCancel,
        ActionAck,
        Hello,
        Ready,
        Heartbeat,
        HeartbeatAck,
        Goodbye
    };
    inline constexpr std::uint8_t FRAME_VERSION{ 6 };
    inline constexpr std::size_t HEADER_BYTES{ 16 };
    inline constexpr std::size_t MAX_FRAME_BYTES{ 2048 };

    // Retains the bytes until transport.send() completes.
    struct OutgoingFrame
    {
        std::array<std::byte, MAX_FRAME_BYTES> bytes{};
        std::size_t size{};
    };

    // Owns the decoded name, but borrows the payload from the receive buffer.
    // Dispatch must finish before that buffer is reused.
    struct IncomingFrame
    {
        FrameKind kind;
        std::string name;
        std::uint64_t id;
        std::uint32_t budget;
        std::span<const std::byte> payload;
    };

    OutgoingFrame make_frame(FrameKind kind,
                             const std::string& name,
                             std::uint64_t id,
                             std::size_t payload_size);
    IncomingFrame decode_frame(std::span<const std::byte> frame);
    void set_request_budget(OutgoingFrame& frame, std::uint32_t milliseconds);

    template<pnm::utils::memory::Serializable T>
    OutgoingFrame encode_frame(FrameKind kind, const std::string& name, std::uint64_t id, const T& value)
    {
        std::size_t size{ sizeof(T) };
        if constexpr (pnm::utils::memory::HasSerializationAdapter<T>) {
            size = pnm::utils::memory::SerializationAdapter<T>::bufferSize(value);
        }
        auto frame = make_frame(kind, name, id, size);
        if (!pnm::utils::memory::serialize(value, std::span(frame.bytes).subspan(frame.size - size, size))) {
            throw std::length_error("Cannot serialize bridge payload");
        }
        return frame;
    }
}
