import os
import subprocess
import sys
import tempfile
simulator,probe=sys.argv[1:3]
with tempfile.TemporaryDirectory(prefix="protomate-storage-") as directory:
    for action in ("write","read"):
        result=subprocess.run([simulator,"--headless","--media-dir",directory,"--controller",probe,"--",action],capture_output=True,text=True,timeout=60)
        assert result.returncode==0,(action,result.returncode,result.stdout,result.stderr)
    assert os.path.getsize(directory+"/flash.img")==16*1024*1024
    assert os.path.getsize(directory+"/sd.img")==64*1024*1024
print("PASS: persistent NOR/SD images, FileX/LevelX file write and restart/readback")
