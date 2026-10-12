#pragma once

#include <pneumo/logging.hpp>

namespace ui::logging
{
    // Local UI records go directly to the console, without passing through ICC.
    // Stateless: queued logger entries may safely retain it after application exit.
    class Sink final : public pnm::log::SinkBase<Sink>
    {
      public:
        bool isOpen() const override;
        pnm::Result<> open() override;
        pnm::Result<> close() override;
        pnm::Result<std::size_t> write(std::span<const std::byte> data) override;
        pnm::Result<> flush() override;
    };
}
