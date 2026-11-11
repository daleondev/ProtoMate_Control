#ifndef PROTOMATE_OPCUA_NETX_H
#define PROTOMATE_OPCUA_NETX_H
#include <open62541/server.h>
#include "nx_api.h"
#ifdef __cplusplus
extern "C" {
#endif
/* The caller owns IP/pool for the entire server lifetime. All UA calls run on
 * one thread. IP callbacks only notify that owner through ThreadX event flags. */
UA_StatusCode opcua_netx_configure(UA_ServerConfig *config, NX_IP *ip, NX_PACKET_POOL *pool);
#ifdef __cplusplus
}
#endif
#endif
