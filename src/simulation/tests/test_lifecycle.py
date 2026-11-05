import ctypes
import os
import selectors
import signal
import subprocess
import time

import pytest
from pathlib import Path

@pytest.fixture
def subreaper():
    libc = ctypes.CDLL(None, use_errno=True)
    previous = ctypes.c_int()
    assert libc.prctl(37, ctypes.byref(previous), 0, 0, 0) == 0
    assert libc.prctl(36, 1, 0, 0, 0) == 0, "PR_SET_CHILD_SUBREAPER failed"
    yield
    assert libc.prctl(36, previous.value, 0, 0, 0) == 0


@pytest.mark.parametrize("failure", [
    "controller-kill", "controller-stall", "simulator-stop", "simulator-kill"])
def test_process_failure_reaps_child(simulator, spawn, subreaper, failure):
    process = spawn([simulator,"--headless"],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,env={**os.environ,"SDL_VIDEODRIVER":"dummy"})
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
        children=Path(f"/proc/{process.pid}/task/{process.pid}/children").read_text().split()
        assert len(children)==1,children
        child=int(children[0])
        if failure=="simulator-stop":process.terminate()
        elif failure=="simulator-kill":process.kill()
        else:os.kill(child,signal.SIGKILL if failure=="controller-kill" else signal.SIGSTOP)
        result=process.wait(8)
        output.extend(process.stdout.read())
        if failure=="controller-kill":assert result==128+signal.SIGKILL,(result,output)
        if failure=="controller-stall":assert result==1 and b"heartbeat timeout" in output,(result,output)
        if failure=="simulator-kill":
            deadline=time.monotonic()+3
            while True:
                reaped,status=os.waitpid(child,os.WNOHANG)
                if reaped:
                    assert os.WIFSIGNALED(status) and os.WTERMSIG(status)==signal.SIGKILL,status
                    break
                assert time.monotonic()<deadline,"controller survived parent death"
                time.sleep(.01)
        assert not os.path.exists(f"/proc/{child}"),"controller was left alive"
    finally:
        if process.poll() is None:
            process.kill();process.wait()
        if child and os.path.exists(f"/proc/{child}"):
            os.kill(child, signal.SIGKILL)
            try:
                os.waitpid(child, 0)
            except ChildProcessError:
                pass
        selector.close()
