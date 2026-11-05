"""The installed artifact must run without its build tree or a sibling controller."""
import os
import hashlib
from pathlib import Path
import selectors
import shutil
import subprocess
import time


def test_self_contained_embedded_controller(simulator, controller, tmp_path, spawn):
    executable=tmp_path/"Application"
    shutil.copy2(simulator,executable)
    environment={key:value for key,value in os.environ.items() if not key.startswith("PROTOMATE_")}
    environment["SDL_VIDEODRIVER"]="dummy"
    process=spawn([str(executable),"--headless"],cwd=tmp_path,env=environment,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    events=selectors.DefaultSelector();events.register(process.stdout,selectors.EVENT_READ)
    output=bytearray()
    try:
        process.stdin.write(b"sim status\n");process.stdin.flush()
        deadline=time.monotonic()+30
        while b"m3 shaft=" not in output:
            assert time.monotonic()<deadline,output.decode(errors="replace")
            for key,_ in events.select(.1):
                block=os.read(key.fileobj.fileno(),8192)
                assert block,output.decode(errors="replace")
                output.extend(block)
        children=Path(f"/proc/{process.pid}/task/{process.pid}/children").read_text().split()
        assert len(children)==1,children
        child=int(children[0])
        assert "memfd:Application-controller" in os.readlink(f"/proc/{child}/exe")
        with open(controller, "rb") as source, open(f"/proc/{child}/exe", "rb") as embedded:
            assert hashlib.file_digest(source, "sha256").digest() == hashlib.file_digest(embedded, "sha256").digest(), "embedded ELF bytes differ from the linked controller"
        assert Path(f"/proc/{process.pid}/exe").resolve()==executable
        assert sorted(path.name for path in tmp_path.iterdir())==["Application"],"executable extracted files"
        process.stdin.close()
        status=process.wait(15)
        output.extend(process.stdout.read())
        assert status==0,(status,output.decode(errors="replace"))
        assert not Path(f"/proc/{child}").exists(),"orphaned controller"
    finally:
        events.close()
