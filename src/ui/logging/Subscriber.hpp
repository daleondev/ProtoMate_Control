#pragma once

#include <pneumo/messaging.hpp>
#include <stop_token>
#include <string_view>

namespace ui::logging
{
    class Subscriber final
    {
      public:
        using Output = std::function<void(std::string_view)>;
        // Subscribe before starting ICC so startup logs can wait for the worker.
        Subscriber(pnm::msg::Bus& bus, Output output);
        // One dedicated worker calls run. Only this worker invokes output;
        // readiness callbacks just wake it. Join it before destroying this object.
        void run(std::stop_token stop);
        pnm::msg::SubscriptionStatistics statistics() const;

      private:
        pnm::msg::Subscription<std::string> m_subscription;
    };
}
