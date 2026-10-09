#pragma once
#include "hal/devices/impl/Lan9253.hpp"

namespace ethercat_echo
{
    // One owner executes the entire SOES stack and all ESC transactions.
    void attach(hal::device::Lan9253& esc);
    void safeOutputs();
}
