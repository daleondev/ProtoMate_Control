#pragma once
#include <pneumo/messaging.hpp>
#include <stop_token>
namespace ui::cli
{
    class Parser;
}
namespace ui::cli::profiling
{
    void setup(Parser& /*parser*/, pnm::msg::Bus& /*bus*/, std::stop_token /*stop*/);
}
