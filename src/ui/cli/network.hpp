#pragma once
#include <stop_token>
namespace ui { class Services; }
namespace ui::opcua { class Network; class Server; }
namespace ui::cli { class Parser; }
namespace ui::cli::network
{
    void setup(Parser& parser, opcua::Network& network, opcua::Server& server, Services& services, std::stop_token stop);
}
