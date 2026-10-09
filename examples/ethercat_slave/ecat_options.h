#pragma once

// Free-running, fixed four-byte PDOs with CoE mailboxes only.
#define USE_FOE 0
#define USE_EOE 0
#define MBXSIZE 128
#define MBXSIZEBOOT 128
#define MBXBUFFERS 3
#define MBX0_sma 0x1000
#define MBX1_sma 0x1080
#define SM2_sma 0x1100
#define SM3_sma 0x1180
#define MAX_RXPDO_SIZE 4
#define MAX_TXPDO_SIZE 4
#define MAX_MAPPINGS_SM2 1
#define MAX_MAPPINGS_SM3 1
