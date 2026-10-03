#include "../system_threads.hpp"

#include <chrono>
#include <thread>

auto system_threads::control() -> void
{
    using namespace std::chrono_literals;
    while (true) {
        std::this_thread::sleep_for(1s);
    }
    std::unreachable();
}
