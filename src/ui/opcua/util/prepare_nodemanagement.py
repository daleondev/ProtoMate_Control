"""Fix 1.5.10's owned method-argument NodeId leak in a build-tree copy.

New argument nodes return owned IDs, while IDs found by browsing are borrowed
from the browse result. Upstream clears only the latter. Track ownership so
string IDs are released on success and failure without freeing borrowed IDs.
"""
from pathlib import Path
import sys

source, output = map(Path, sys.argv[1:])
text = source.read_text()
begin = text.index("UA_Server_addMethodNodeEx_finish(")
end = text.index("\nUA_StatusCode\nUA_Server_addMethodNode_finish", begin)
section = text[begin:end]

def replace_once(old, new):
    global section
    if section.count(old) != 1:
        raise SystemExit("open62541 method-node ownership changed; review the compatibility fix")
    section = section.replace(old, new)

replace_once("    UA_NodeId outputArgsId = UA_NODEID_NULL;",
             "    UA_NodeId outputArgsId = UA_NODEID_NULL;\n"
             "    UA_Boolean inputArgsOwned = false, outputArgsOwned = false;")
for name in ("input", "output"):
    replace_once(f"        retval = addNode(server, UA_NODECLASS_VARIABLE, {name}ArgumentsRequestedNewNodeId,",
                 f"        {name}ArgsOwned = true;\n"
                 f"        retval = addNode(server, UA_NODECLASS_VARIABLE, {name}ArgumentsRequestedNewNodeId,")
cleanup = ("    if(inputArgsOwned) UA_NodeId_clear(&inputArgsId);\n"
           "    if(outputArgsOwned) UA_NodeId_clear(&outputArgsId);\n")
replace_once("    UA_BrowseResult_clear(&br);\n    return retval;\n\nerror:",
             cleanup + "    UA_BrowseResult_clear(&br);\n    return retval;\n\nerror:")
replace_once("    deleteNode(server, outputArgsId, true);\n",
             "    deleteNode(server, outputArgsId, true);\n" + cleanup)
text = text[:begin] + section + text[end:]
if not output.exists() or output.read_text() != text:
    output.write_text(text)
