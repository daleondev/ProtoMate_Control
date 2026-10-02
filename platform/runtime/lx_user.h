#pragma once

// Keep the hot LevelX lookup tables in RAM.  The mapping bitmap is 4 KiB for
// the 16 MiB W25Q128 and prevents a full flash scan for sectors that do not
// exist; the obsolete-count cache makes block reclamation bounded as well.
#define LX_NOR_ENABLE_MAPPING_BITMAP
#define LX_NOR_ENABLE_OBSOLETE_COUNT_CACHE

// FileX serializes all access to a media instance.  Avoid a second nested
// ThreadX mutex in LevelX; the storage adapter also owns the physical device.

