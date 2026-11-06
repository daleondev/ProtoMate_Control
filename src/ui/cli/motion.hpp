#pragma once
#include <stop_token>
namespace ui::motion { class Client; class Robot; }
namespace ui::cli { class Parser; }
namespace ui::cli::motion {
    void setup(Parser& parser, ui::motion::Client& client, ui::motion::Robot& robot, std::stop_token stop);
}
