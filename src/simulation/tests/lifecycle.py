import os
import selectors
import signal
import subprocess
import sys
import time

simulator, controller = sys.argv[1:3]
for failure in ("controller-kill", "controller-stall", "simulator-stop"):
    process = subprocess.Popen([simulator,"--headless","--controller",controller],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,env={**os.environ,"SDL_VIDEODRIVER":"dummy"})
    selector=selectors.DefaultSelector();selector.register(process.stdout,selectors.EVENT_READ)
    output=bytearray();child=None
    try:
        deadline=time.monotonic()+30
        while b"> " not in output:
            assert time.monotonic()<deadline,"startup timeout"
            for key,_ in selector.select(.1):
                block=os.read(key.fileobj.fileno(),8192)
                assert block,output.decode(errors="replace")
                output.extend(block)
        children=open(f"/proc/{process.pid}/task/{process.pid}/children").read().split()
        assert len(children)==1,children
        child=int(children[0])
        if failure=="simulator-stop":process.terminate()
        else:os.kill(child,signal.SIGKILL if failure=="controller-kill" else signal.SIGSTOP)
        result=process.wait(8)
        output.extend(process.stdout.read())
        if failure=="controller-kill":assert result==128+signal.SIGKILL,(result,output)
        if failure=="controller-stall":assert result==1 and b"heartbeat timeout" in output,(result,output)
        assert not os.path.exists(f"/proc/{child}"),"controller was left alive"
    finally:
        if process.poll() is None:
            process.kill();process.wait()
        if child and os.path.exists(f"/proc/{child}"):os.kill(child,signal.SIGKILL)
        selector.close()
print("PASS: killed/stalled controller and simulator shutdown release the child without replay")
