#include "esc_coe.h"
#include "device.h"

uint32_t command_value;
uint32_t echo_value;

static const _objd device_type[] = {{0, DTYPE_UNSIGNED32, 32, ATYPE_RO, "Device type", 0, NULL}};
static const _objd device_name[] = {{0, DTYPE_VISIBLE_STRING, (sizeof(EC_DEVICE_NAME)-1)*8, ATYPE_RO, "Name", 0, EC_DEVICE_NAME}};
static const _objd identity[] = {
    {0, DTYPE_UNSIGNED8, 8, ATYPE_RO, "Entries", 4, NULL},
    {1, DTYPE_UNSIGNED32, 32, ATYPE_RO, "Vendor", EC_VENDOR_ID, NULL},
    {2, DTYPE_UNSIGNED32, 32, ATYPE_RO, "Product", EC_PRODUCT_CODE, NULL},
    {3, DTYPE_UNSIGNED32, 32, ATYPE_RO, "Revision", EC_REVISION, NULL},
    {4, DTYPE_UNSIGNED32, 32, ATYPE_RO, "Serial", 0, NULL},
};
static const _objd rx_mapping[] = {
    {0, DTYPE_UNSIGNED8, 8, ATYPE_RO, "Entries", 1, NULL},
    {1, DTYPE_UNSIGNED32, 32, ATYPE_RO, "Value mapping", 0x70000120, NULL},
};
static const _objd tx_mapping[] = {
    {0, DTYPE_UNSIGNED8, 8, ATYPE_RO, "Entries", 1, NULL},
    {1, DTYPE_UNSIGNED32, 32, ATYPE_RO, "Echo mapping", 0x60000120, NULL},
};
static const _objd sm_types[] = {
    {0, DTYPE_UNSIGNED8, 8, ATYPE_RO, "Entries", 4, NULL},
    {1, DTYPE_UNSIGNED8, 8, ATYPE_RO, "Mailbox out", 1, NULL},
    {2, DTYPE_UNSIGNED8, 8, ATYPE_RO, "Mailbox in", 2, NULL},
    {3, DTYPE_UNSIGNED8, 8, ATYPE_RO, "Outputs", 3, NULL},
    {4, DTYPE_UNSIGNED8, 8, ATYPE_RO, "Inputs", 4, NULL},
};
static const _objd rx_assignment[] = {
    {0, DTYPE_UNSIGNED8, 8, ATYPE_RO, "Entries", 1, NULL},
    {1, DTYPE_UNSIGNED16, 16, ATYPE_RO, "PDO", 0x1600, NULL},
};
static const _objd tx_assignment[] = {
    {0, DTYPE_UNSIGNED8, 8, ATYPE_RO, "Entries", 1, NULL},
    {1, DTYPE_UNSIGNED16, 16, ATYPE_RO, "PDO", 0x1a00, NULL},
};
static const _objd response[] = {
    {0, DTYPE_UNSIGNED8, 8, ATYPE_RO, "Entries", 1, NULL},
    {1, DTYPE_UNSIGNED32, 32, ATYPE_RO | ATYPE_TXPDO, "Echo", 0, &echo_value},
};
static const _objd command[] = {
    {0, DTYPE_UNSIGNED8, 8, ATYPE_RO, "Entries", 1, NULL},
    {1, DTYPE_UNSIGNED32, 32, ATYPE_RO | ATYPE_RXPDO, "Value", 0, &command_value},
};

const _objectlist SDOobjects[] = {
    {0x1000, OTYPE_VAR, 0, 0, "Device type", device_type},
    {0x1008, OTYPE_VAR, 0, 0, "Device name", device_name},
    {0x1018, OTYPE_RECORD, 4, 0, "Identity", identity},
    {0x1600, OTYPE_RECORD, 1, 0, "Command mapping", rx_mapping},
    {0x1a00, OTYPE_RECORD, 1, 0, "Response mapping", tx_mapping},
    {0x1c00, OTYPE_ARRAY, 4, 0, "SM types", sm_types},
    {0x1c12, OTYPE_ARRAY, 1, 0, "RxPDO assignment", rx_assignment},
    {0x1c13, OTYPE_ARRAY, 1, 0, "TxPDO assignment", tx_assignment},
    {0x6000, OTYPE_RECORD, 1, 0, "Response", response},
    {0x7000, OTYPE_RECORD, 1, 0, "Command", command},
    {0xffff, 0xff, 0xff, 0xff, NULL, NULL},
};
