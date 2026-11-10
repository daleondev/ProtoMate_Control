#pragma once
#include <stop_token>
namespace ui::gui::os
{
    // Construct before the TouchGFX engine members; destruction runs after them
    // and also releases synchronization when engine construction throws.
    class Session
    {
      public:
        explicit Session(std::stop_token stop);
        ~Session();
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;
        Session(Session&&) = delete;
        Session& operator=(Session&&) = delete;
    };
}
