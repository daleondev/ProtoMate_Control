#ifndef PROTOMATE_WIFI_H
#define PROTOMATE_WIFI_H
#include "nx_api.h"
#ifdef __cplusplus
extern "C" {
#endif
int wifi_initialize(void);
int wifi_join(const char *ssid, const char *password);
int wifi_connected(void);
int wifi_disconnect(void);
int wifi_reconnect(void); /* Uses the last successful credentials, RAM only. */
typedef struct { char ssid[33]; int rssi; unsigned channel; } WifiScanEntry;
typedef struct { WifiScanEntry entries[16]; unsigned count, omitted; int running; } WifiScan;
int wifi_scan_start(void);
void wifi_scan_snapshot(WifiScan *result);
NX_IP *wifi_ip(void);
NX_PACKET_POOL *wifi_packet_pool(void);
#ifdef __cplusplus
}
#endif
#endif
