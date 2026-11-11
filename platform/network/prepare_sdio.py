"""Bound cache operations to the aligned SDIO bounce buffer (SDK 1.8.0).

The upstream read path invalidates length+32 bytes, crossing the end of the
buffer for a full-size transfer. Round up, without adding an extra cache line.
Read completion invalidates DMA-written lines rather than cleaning them back.
All changes live in build output; the pinned vendor tree remains unchanged.
"""
from pathlib import Path
import sys

source, output = map(Path, sys.argv[1:])
text = source.read_text()
needle = "length + __SCB_DCACHE_LINE_SIZE"
if text.count(needle) != 2 or text.count("SCB_CleanInvalidateDCache_by_Addr(") != 1:
    raise SystemExit("SDIO cache implementation changed; review adaptation")
text = text.replace(needle, "((length + __SCB_DCACHE_LINE_SIZE - 1U) & ~(__SCB_DCACHE_LINE_SIZE - 1U))")
text = text.replace("SCB_CleanInvalidateDCache_by_Addr(", "SCB_InvalidateDCache_by_Addr(")
if not output.exists() or output.read_text() != text:
    output.write_text(text)
