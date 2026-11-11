#include "Wifi.h"
#include "Memory.h"
#include "cy_wcm.h"
#include "cy_network_mw_core.h"
#include "whd_wifi_api.h"
#include <string.h>

int wifi_board_initialize(void);
/* Package 1.8.0 exports this accessor in cy_wcm.c but omits its declaration
 * from cy_wcm.h. Keep this version-specific seam local to the platform port. */
cy_rslt_t cy_wcm_get_whd_interface(cy_wcm_interface_t type, whd_interface_t *interface);
static cy_wcm_connect_params_t retained_credentials;
static int have_credentials;
static WifiScan scan;
static int initialization_attempted;
static int initialization_result;

int wifi_initialize(void) {
    /* The network owner calls this from one worker. Never clear live pools on
     * a later start/restart request, including after a partial SDK failure. */
    if(initialization_attempted) return initialization_result;
    initialization_attempted = 1;
    initialization_result = -4;
    /* NOLOAD network statics are owned here, not cleared after workers start. */
    extern unsigned char __network_sdram_start[], __network_sdram_end[];
    for(volatile unsigned char *p=__network_sdram_start; p<__network_sdram_end; ++p) *p=0;
    if(!network_memory_initialize()) return initialization_result;
    int status = wifi_board_initialize();
    if(status) return initialization_result = status;
    cy_wcm_config_t config = {.interface = CY_WCM_INTERFACE_TYPE_STA};
    status = (int)cy_wcm_init(&config);
    if(status) return initialization_result = status;
    whd_interface_t interface;
    status = (int)cy_wcm_get_whd_interface(CY_WCM_INTERFACE_TYPE_STA, &interface);
    if(status) return initialization_result = status;
    return initialization_result = (int)whd_wifi_disable_powersave(interface);
}

int wifi_join(const char *ssid, const char *password) {
    if(!ssid || !password || !*ssid || strlen(ssid) > 32 || strlen(password) < 8 || strlen(password) > 63)
        return -1;
    cy_wcm_connect_params_t params = {0};
    memcpy(params.ap_credentials.SSID, ssid, strlen(ssid));
    memcpy(params.ap_credentials.password, password, strlen(password));
    params.ap_credentials.security = CY_WCM_SECURITY_WPA2_AES_PSK;
    cy_wcm_ip_address_t address;
    int status = (int)cy_wcm_connect_ap(&params, &address);
    if(status == 0) { retained_credentials = params; have_credentials = 1; }
    volatile unsigned char *clear = (volatile unsigned char *)&params;
    for(size_t i = 0; i < sizeof(params); ++i) clear[i] = 0;
    return status;
}

int wifi_connected(void) { return cy_wcm_is_connected_to_ap(); }
int wifi_disconnect(void) { return (int)cy_wcm_disconnect_ap(); }
void wifi_forget_credentials(void) {
    have_credentials = 0;
    volatile unsigned char *clear = (volatile unsigned char *)&retained_credentials;
    for(size_t i=0; i<sizeof(retained_credentials); ++i) clear[i]=0;
}
int wifi_reconnect(void) {
    if(!have_credentials) return -1;
    cy_wcm_ip_address_t address;
    return (int)cy_wcm_connect_ap(&retained_credentials, &address);
}
static void scan_result(cy_wcm_scan_result_t *entry, void *context, cy_wcm_scan_status_t state) {
    (void)context;
    UINT previous = tx_interrupt_control(TX_INT_DISABLE);
    if(state == CY_WCM_SCAN_COMPLETE) scan.running = 0;
    else if(entry && scan.count < 16) {
        WifiScanEntry *target = &scan.entries[scan.count++];
        memcpy(target->ssid, entry->SSID, 32);
        target->ssid[32] = 0;
        /* SSIDs are network-provided bytes, never terminal control sequences. */
        for(unsigned i=0; i<32 && target->ssid[i]; ++i)
            if((unsigned char)target->ssid[i]<32 || (unsigned char)target->ssid[i]==127) target->ssid[i]='?';
        target->rssi = entry->signal_strength;
        target->channel = entry->channel;
    } else if(entry) ++scan.omitted;
    tx_interrupt_control(previous);
}
int wifi_scan_start(void) {
    UINT previous = tx_interrupt_control(TX_INT_DISABLE);
    if(scan.running) { tx_interrupt_control(previous); return -1; }
    scan = (WifiScan){.running=1};
    tx_interrupt_control(previous);
    int result = (int)cy_wcm_start_scan(scan_result, NULL, NULL);
    if(result) { previous=tx_interrupt_control(TX_INT_DISABLE);scan.running=0;tx_interrupt_control(previous); }
    return result;
}
void wifi_scan_snapshot(WifiScan *result) {
    UINT previous = tx_interrupt_control(TX_INT_DISABLE);
    *result = scan;
    tx_interrupt_control(previous);
}
int wifi_scan_stop(void) { return (int)cy_wcm_stop_scan(); }
NX_IP *wifi_ip(void) { return cy_network_get_nw_interface(CY_NETWORK_WIFI_STA_INTERFACE, 0); }
NX_PACKET_POOL *wifi_packet_pool(void) {
    NX_IP *ip = wifi_ip();
    return ip ? ip->nx_ip_default_packet_pool : NULL;
}
