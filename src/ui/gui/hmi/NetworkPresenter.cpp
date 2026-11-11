#include "Presenter.hpp"
#include <cstring>

namespace ui::gui::hmi
{
    void Presenter::editText(char key)
    {
        auto& form{m_model.network};
        if (!form.editing) return;
        const auto length{std::strlen(form.editor.data())};
        const auto capacity{form.editing == 1 ? form.ssid.size() : form.password.size()};
        if (key == '\x1b') form.clearEditor();
        else if (key == '\b' || key == '\x7f') { if (length) form.editor[length - 1] = 0; }
        else if (key == '\n') {
            if (length && (form.editing == 1 || length >= 8)) {
                if (form.editing == 1) {form.ssid.fill(0); std::copy_n(form.editor.begin(), length, form.ssid.begin());}
                else form.password = form.editor;
                form.clearEditor();
            } else m_model.report("SSID is required; WPA2 password requires 8–63 characters", Severity::Warning);
        } else if (key >= ' ' && key <= '~' && length + 1 < capacity) {
            form.editor[length] = key; form.editor[length + 1] = 0;
        }
        m_pending.changed = true;
    }
    bool Presenter::networkActions(Action a)
    {
        auto& form{m_model.network};
        if (a >= TextFirst && a <= TextLast) {
            const auto character{form.key_page * 18 + (a - TextFirst)};
            if (character < NETWORK_KEYS.size()) editText(NETWORK_KEYS[character]);
            return true;
        }
        if (a == SettingsNetwork) { releaseHeld(); m_model.settings_tab = 3; m_pending.changed = true; return true; }
        if (a == NetworkSSID || a == NetworkPassword) {
            releaseHeld(); form.clearEditor(); form.editing = a == NetworkSSID ? 1 : 2; form.key_page = 0;
            if (++m_model.editor_revision == 0) ++m_model.editor_revision;
            m_pending.changed = true; return true;
        }
        switch (a) {
            case TextSpace: editText(' '); return true;
            case TextDelete: editText('\b'); return true;
            case TextApply: editText('\n'); return true;
            case TextCancel: editText('\x1b'); return true;
            case TextPrevious: form.key_page = (form.key_page + 5) % 6; m_pending.changed = true; return true;
            case TextNext: form.key_page = (form.key_page + 1) % 6; m_pending.changed = true; return true;
            case NetworkScanNext: form.scan_page = (form.scan_page + 1) % 6; m_pending.changed = true; return true;
            default: break;
        }
        if (!m_model.backend) return false;
        if (a >= NetworkEntry0 && a <= NetworkEntry2) {
            NetworkEntry entry{};
            if (m_model.backend->networkEntry(form.scan_page * 3 + (a - NetworkEntry0), entry)) form.ssid = entry.ssid;
            m_pending.changed = true; return true;
        }
        bool accepted{};
        switch (a) {
            case NetworkConnect:
                accepted = form.ssid[0] ? m_model.backend->configureNetwork(form.ssid.data(), form.password.data())
                                       : m_model.backend->networkAction(NetworkAction::Connect);
                if (accepted) form.clear();
                break;
            case NetworkDisconnect: accepted = m_model.backend->networkAction(NetworkAction::Disconnect); break;
            case NetworkForget: accepted = m_model.backend->networkAction(NetworkAction::Forget); form.clear(); break;
            case NetworkScan: accepted = m_model.backend->networkAction(NetworkAction::Scan); form.scan_page = 0; break;
            case RemoteAllow: accepted = m_model.backend->networkAction(NetworkAction::AllowRemote); break;
            case RemoteRevoke: accepted = m_model.backend->networkAction(NetworkAction::RevokeRemote); break;
            case ServerStart: accepted = m_model.backend->networkAction(NetworkAction::StartServer); break;
            case ServerStop: accepted = m_model.backend->networkAction(NetworkAction::StopServer); break;
            default: return false;
        }
        m_model.report(accepted ? "Local network/control request accepted" : "Request rejected: check credentials or wait for network manager",
            accepted ? Severity::Info : Severity::Warning);
        m_pending.changed = true;
        return true;
    }
}
