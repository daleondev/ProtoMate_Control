#pragma once

/* GIGA STM32H747 bank 2 sectors 4..7. Bootstraps occupy sector 0 of
 * each bank. Firmware updates must preserve this entire partition. */
#define STORAGE_INTERNAL_ORIGIN 0x08180000u
#define STORAGE_INTERNAL_BYTES  0x00080000u
#define STORAGE_INTERNAL_ERASE_BYTES 0x00020000u
#define STORAGE_INTERNAL_FIRST_SECTOR 4u
