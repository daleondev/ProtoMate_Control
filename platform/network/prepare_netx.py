"""Keep Infineon's ThreadX network priority below UI/ICC without vendor edits."""
from pathlib import Path
import sys

source, output = map(Path, sys.argv[1:])
text = source.read_text()
needle = '#define IP_THREAD_PRIORITY                  (2)'
if text.count(needle) != 1:
    raise SystemExit('Infineon IP priority changed; review adaptation')
text = text.replace(needle, '#define IP_THREAD_PRIORITY                  (10)')
if not output.exists() or output.read_text() != text:
    output.write_text(text)
