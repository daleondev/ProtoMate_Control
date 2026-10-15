#pragma once
#include <stop_token>
namespace ui::gui::os
{
    void prepare(std::stop_token stop);
    void finish();
}
