#include "format.h"

uint32_t giga_crc32(const void *data, size_t size)
{
    const uint8_t *bytes = data;
    uint32_t crc = UINT32_MAX;
    while (size--) {
        crc ^= *bytes++;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}
static int within(uint32_t address, uint32_t size, uint32_t begin, uint32_t length)
{
    return address >= begin && size <= length && address - begin <= length - size;
}
static int destination(unsigned core, uint32_t address, uint32_t size, uint32_t flags)
{
    if (!core) {
        return within(address, size, 0x10000000, 128 * 1024) ||
               within(address, size, 0x30020000, 152 * 1024) ||
               ((flags & GIGA_RAM_DEBUG) && within(address, size, 0xC0400000, 2 * 1024 * 1024));
    }
    /* Keep bootstrap scratch (0x2001C000) and both MSP stacks intact. */
    return within(address, size, 0, 64 * 1024) ||
           within(address, size, 0x20000000, 112 * 1024) ||
           within(address, size, 0x24000000, 256 * 1024);
}
int giga_manifest_valid(const GigaManifest *m)
{
    if (m->magic != GIGA_MANIFEST_MAGIC || m->version != GIGA_FORMAT_VERSION ||
        m->size != sizeof(*m) || m->crc != giga_crc32((const uint8_t *)m + 16, sizeof(*m) - 16)) {
        return 0;
    }
    for (unsigned core = 0; core < 2; ++core) {
        const GigaImage *im = &m->images[core];
        const uint32_t base = core ? 0x90400000u : 0x90100000u;
        const uint32_t limit = core ? 4u * 1024 * 1024 : 3u * 1024 * 1024;
        if (im->storage != base || im->size <= sizeof(m->pair_id) || im->size > limit ||
            im->segment_count == 0 || im->segment_count > GIGA_MAX_SEGMENTS ||
            (im->flags & ~GIGA_RAM_DEBUG) || (core && im->flags) || !(im->entry & 1u) ||
            im->vectors != (core ? 0x20000000u : 0x10000000u) ||
            im->stack != (core ? 0x20020000u : 0x30048000u)) { return 0; }
        const uint32_t pc = im->entry & ~1u;
        if (!within(pc, 2, base, im->size) &&
            !destination(core, pc, 2, im->flags)) { return 0; }
        int vectors = 0, entry = within(pc, 2, base, im->size);
        for (uint32_t i = 0; i < im->segment_count; ++i) {
            const GigaSegment *s = &im->segments[i];
            if (!s->size || !destination(core, s->run, s->size, im->flags) ||
                (s->flags != GIGA_COPY && s->flags != GIGA_ZERO) ||
                (s->flags == GIGA_COPY && !within(s->load, s->size, base, im->size)) ||
                (s->flags == GIGA_ZERO && s->load)) { return 0; }
            if (s->flags == GIGA_COPY && within(im->vectors, 8, s->run, s->size)) { vectors = 1; }
            if (s->flags == GIGA_COPY && within(pc, 2, s->run, s->size)) { entry = 1; }
            for (uint32_t j = 0; j < i; ++j) {
                const GigaSegment *p = &im->segments[j];
                if (s->run < p->run + p->size && p->run < s->run + s->size) { return 0; }
            }
        }
        if (!vectors || !entry) { return 0; }
    }
    return 1;
}
