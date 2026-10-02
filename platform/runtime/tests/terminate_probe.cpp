#include "hal/hal.hpp"

#include <exception>
#include <new>
#include <stdexcept>
#include <string_view>
#include <thread>

int main(int argc, char** argv)
{
    if (argc == 2 && std::string_view{ argv[1] } == "error_handler") {
        Error_Handler();
    }

    if (argc == 2 && std::string_view{ argv[1] } == "thread") {
        std::thread throwing_thread{ [] { throw std::runtime_error{ "uncaught thread exception" }; } };
        throwing_thread.join();
        return 1;
    }

    if (argc == 2 && std::string_view{ argv[1] } == "non_std") {
        throw 42;
    }

    if (argc == 2 && std::string_view{ argv[1] } == "bad_alloc") {
        throw std::bad_alloc{};
    }

    if (argc == 2 && std::string_view{ argv[1] } == "explicit") {
        std::terminate();
    }

    throw std::runtime_error{ "uncaught application exception" };
}
