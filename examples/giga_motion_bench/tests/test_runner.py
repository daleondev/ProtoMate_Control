import importlib.util
import io
import json
from pathlib import Path
import pytest

spec=importlib.util.spec_from_file_location('motion_bench_runner',Path(__file__).parents[1]/'run.py')
runner=importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)

class Port:
    def __init__(self,lines):self.lines=iter(lines);self.writes=[]
    def write(self,data):self.writes.append(data)
    def flush(self):pass
    def readline(self):return next(self.lines,b'')

def test_waits_for_completion_and_preserves_bad_results():
    data={'test':'raw','errors':2,'latency':{'max_us':90000}}
    port=Port([json.dumps(data).encode(),b'[motion-bench] DONE'])
    records,lines=runner.transact(port,'raw 32 100 4 1 0')
    assert records==[data]
    assert port.writes==[b'raw 32 100 4 1 0\n']

@pytest.mark.parametrize('message',[b'[motion-bench] invalid',b'[motion-bench] invalid phase',b'panic: fault'])
def test_rejection_is_not_a_success(message):
    with pytest.raises(RuntimeError):runner.transact(Port([message]),'bad')

def test_malformed_export_keeps_original_console_line():
    log=io.StringIO()
    with pytest.raises(json.JSONDecodeError):runner.transact(Port([b'{partial json']), 'clock',log=log)
    assert log.getvalue()=='{partial json\n'

def test_no_completion_is_timeout():
    with pytest.raises(TimeoutError):runner.transact(Port([]),'clock',timeout=.001)

@pytest.mark.parametrize('layer',['raw','transport','bridge'])
def test_sweep_uses_valid_sizes_and_directions(layer):
    for command in runner.cases(layer):
        name,size,rate,count,direction,lane=command.split()
        assert name==layer and 16<=int(size)<=2048
        assert int(rate) in (100,1000,5000,10000)
        assert int(count)>0
        assert int(direction) in ((1,) if layer=='bridge' else (0,1,2))
        assert lane=='0'
