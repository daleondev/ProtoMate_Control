#pragma once
#include <pneumo/messaging.hpp>
#include <stop_token>
namespace ui::cli
{
    class Parser;
}
namespace ui::cli::profiling
{
    void setup(Parser&, pnm::msg::Bus&, std::stop_token);
}
