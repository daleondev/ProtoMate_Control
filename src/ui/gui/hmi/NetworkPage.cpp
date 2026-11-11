#include "View.hpp"
#include <cstdio>
#include <cstring>

namespace ui::gui::hmi
{
    void View::networkDialog()
    {
        const auto& form{m_model.network};
        label(form.editing == 1 ? "Wi-Fi SSID" : "WPA2 password (hidden)", 136, 90, 816, 36, true);
        std::array<char, 64> display{};
        if (form.editing == 1) display = form.editor;
        else format(display, "{} ({} characters)", form.editor[0] ? "********" : "", std::strlen(form.editor.data()));
        label(display.data(), 136, 145, 816, 48, true, touchgfx::CENTER);
        label("Saved locally after connection. Passwords are not encrypted at rest.", 136, 202, 816);
        for (unsigned i{}; i < 18; ++i) {
            const unsigned index{form.key_page * 18 + i};
            const std::array<char,2> caption{index < NETWORK_KEYS.size() ? NETWORK_KEYS[index] : ' ', 0};
            button(TextFirst + i, caption[0] == ' ' ? "Space" : caption.data(), 136 + (i % 6) * 144, 242 + (i / 6) * 64, 132, 56, index < NETWORK_KEYS.size());
        }
        constexpr std::array ids{TextSpace, TextPrevious, TextNext, TextDelete, TextApply, TextCancel};
        constexpr std::array names{"Space", "Previous", "Next", "Delete", "Apply", "Cancel"};
        for (unsigned i{}; i < ids.size(); ++i) button(ids[i], names[i], 136 + i * 144, 446, 132);
    }
    void View::networkPage()
    {
        const auto status{m_model.backend ? m_model.backend->networkStatus() : NetworkStatus{}};
        auto& form{m_model.network};
        std::array<char,128> text{};
        format(text, "{} · {} · {} client(s)", status.connected ? "Connected" : "Offline",
            status.server ? "OPC UA running" : "Server stopped", status.clients);
        label(text.data(), 136, 152, 816, 32, true);
        if (status.wifi) format(text, "opc.tcp://{}.{}.{}.{}:4840 · {}", status.address >> 24,
            (status.address >> 16) & 255, (status.address >> 8) & 255, status.address & 255, status.saved ? "Profile saved" : "No saved profile");
        else format(text, "opc.tcp://<host-address>:4840 · Linux uses the operating-system network");
        label(text.data(), 136, 188, 816);
        button(status.remote ? RemoteRevoke : RemoteAllow, status.remote ? "Revoke remote control" : "Allow remote control", 136, 226, 400, 56, true, status.remote);
        button(status.server ? ServerStop : ServerStart, status.server ? "Stop server" : "Start server", 552, 226, 424);
        if (!status.wifi) { label("Wi-Fi credentials are managed by Linux. Remote permission starts OFF after every boot.", 136, 312, 816, 64); return; }
        format(text, "SSID: {:.32}", form.ssid[0] ? form.ssid.data() : status.ssid.data());
        button(NetworkSSID, text.data(), 136, 294, 400, 56, !status.busy);
        button(NetworkPassword, form.password[0] ? "Password: ********" : "Enter password", 552, 294, 424, 56, !status.busy);
        button(NetworkConnect, "Connect / save", 136, 358, 196, 56, !status.busy);
        button(NetworkDisconnect, "Disconnect", 344, 358, 196, 56, !status.busy);
        button(NetworkForget, "Forget profile", 552, 358, 196, 56, !status.busy);
        button(NetworkScan, "Scan Wi-Fi", 760, 358, 216, 56, !status.busy);
        for (unsigned i{}; i < 3; ++i) {
            NetworkEntry entry{};
            const bool valid{m_model.backend && m_model.backend->networkEntry(form.scan_page * 3 + i, entry)};
            if (valid) format(text, "{:.22} {}", entry.ssid.data(), entry.signal);
            else format(text, "No network");
            button(NetworkEntry0 + i, text.data(), 136 + i * 216, 426, 204, 56, valid && !status.busy);
        }
        button(NetworkScanNext, "Next", 784, 426, 192, 56, status.scan_count > 3);
        format(text, "{} · error {} · scan error {} · {} networks ({} omitted)", status.busy ? "Working…" : "Ready",
            status.error, status.scan_error, status.scan_count, status.scan_omitted);
        label(text.data(), 136, 488, 816);
    }
}
