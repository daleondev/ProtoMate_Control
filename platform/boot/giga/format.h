#pragma once
#include <stdint.h>
#include <stddef.h>

#define GIGA_MANIFEST_ADDRESS 0x90E00000u
#define GIGA_MANIFEST_MAGIC 0x41474947u
#define GIGA_FORMAT_VERSION 1u
#define GIGA_MAX_SEGMENTS 16u
#define GIGA_COPY 1u
#define GIGA_ZERO 2u
#define GIGA_RAM_DEBUG 1u
#define GIGA_HANDOFF_ADDRESS 0x3800F000u
#define GIGA_HANDOFF_MAGIC 0x47424F54u

/* Wire format: little-endian 32-bit words. No pointers or native-size fields. */
typedef struct { uint32_t load, run, size, flags; } GigaSegment;
typedef struct {
    uint32_t storage, size, crc, entry, vectors, stack, segment_count, flags;
    GigaSegment segments[GIGA_MAX_SEGMENTS];
} GigaImage;
typedef struct {
    uint32_t magic, version, size, crc;
    uint32_t pair_id[8];
    GigaImage images[2]; /* M4, M7 */
} GigaManifest;
typedef struct {
    uint32_t magic, m4_entry, m4_vectors, m4_stack, m4_flags, status;
    uint32_t qspi_id, qspi_sfdp, memory_test, reserved[7];
} GigaHandoff;

#ifdef __cplusplus
extern "C" {
#endif
uint32_t giga_crc32(const void *data, size_t size);
int giga_manifest_valid(const GigaManifest *manifest);
int giga_image_valid(const GigaImage *image, const uint32_t pair_id[8], const void *payload, size_t available);
#ifdef __cplusplus
}
static_assert(sizeof(GigaManifest) == 624);
#else
_Static_assert(sizeof(GigaManifest) == 624, "Manifest layout changed");
#endif
