#pragma once

#include <pneumo/messaging.hpp>

#include <string>

namespace example
{
    using namespace pnm::meta::string::literals;

    enum class ServiceProvider
    {
        IO,
        UI
    };

    // Fields declare request/response types; no Services instance is constructed.
    // IO provides this service; UI calls it. Replies return to the caller.
    struct Services
    {
        [[= ServiceProvider::IO]] pnm::msg::Service<std::string, std::string> demo_io_echo;
    };
}
