#pragma once
#include "hal/util/Result.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
namespace hal::graphics
{
    enum class Format
    {
        Rgb565,
        Argb8888
    };
    enum class Operation
    {
        Fill,
        Copy,
        Blend,
        BlendFill
    };
    struct Transfer
    {
        Operation operation{};
        // Destination is RGB565. Strides are bytes, including the written row.
        std::span<std::byte> destination;
        std::size_t destinationStride{};
        std::span<const std::byte> source;
        std::size_t sourceStride{};
        Format sourceFormat{ Format::Rgb565 };
        unsigned width{}, height{};
        std::uint32_t color{};     // ARGB8888 for Fill/BlendFill
        std::uint8_t alpha{ 255 }; // combined with pixel alpha for Blend
    };
    enum class Completion
    {
        Complete,
        Error,
        Aborted
    };
    struct Callback
    {
        void (*function)(void*, Completion) noexcept {};
        void* context{};
        void operator()(Completion completion) const noexcept
        {
            if (function) {
                function(context, completion);
            }
        }
    };
}
namespace hal
{
    class IGraphics
    {
      public:
        virtual ~IGraphics() = default;
        // One operation; buffers remain owned by the operation until callback.
        // A cached DMA destination must own complete cache lines (32-byte
        // aligned address and span size on STM32). Source/destination must not
        // overlap. Framebuffers are non-cacheable and may use odd pixel offsets.
        // Callback may be inline or in an ISR and must only notify a worker.
        virtual auto submit(const graphics::Transfer& transfer, graphics::Callback callback)
          -> util::Result<> = 0;
        // Synchronously abort and remove callback before releasing its context.
        virtual void abort() noexcept = 0;
        virtual bool busy() const noexcept = 0;
        virtual std::uint32_t errors() const noexcept = 0;
    };
}
