"""Apply the small, checked open62541 1.5.10 architecture adaptation in build output."""
from pathlib import Path
import sys

source, generated, output = map(Path, sys.argv[1:])
header = generated / "open62541/config.h"
text = header.read_text()
needle = "#if !defined(UA_ARCHITECTURE_WIN32) && !defined(UA_ARCHITECTURE_POSIX) &&"
if text.count(needle) != 1:
    raise SystemExit("open62541 architecture selection changed; review the NetX port")
text = text.replace(needle, "#if !defined(UA_ARCHITECTURE_NETX) && !defined(UA_ARCHITECTURE_WIN32) && !defined(UA_ARCHITECTURE_POSIX) &&")
adapted_header = output.parent / "netx-include/open62541/config.h"
adapted_header.parent.mkdir(parents=True, exist_ok=True)
if not adapted_header.exists() or adapted_header.read_text() != text:
    adapted_header.write_text(text)

text = (source / "plugins/ua_config_default.c").read_text()
for name in ("conf", "config"):
    marker = f"    if({name}->eventLoop == NULL) {{"
    begin = text.index(marker)
    cursor = begin + len(marker)
    depth = 1
    while depth:
        if text[cursor] == "{":
            depth += 1
        elif text[cursor] == "}":
            depth -= 1
        cursor += 1
    # A caller-supplied event loop is mandatory on NetX. No POSIX constructors
    # (including unused UDP/interrupt managers) may remain as link references.
    text = text[:begin] + marker + "\n        return UA_STATUSCODE_BADCONFIGURATIONERROR;\n    }" + text[cursor:]
if not output.exists() or output.read_text() != text:
    output.write_text(text)
