"""Real SDL input -> TouchGFX -> UI client -> ICC -> IO -> native plant."""
import math
import json
import os
import re
import selectors
import subprocess
import time


def test_hmi_motion_workflow(simulator,tmp_path,spawn):
    fifo=tmp_path/'input.fifo';os.mkfifo(fifo)
    pipe=os.open(fifo,os.O_RDWR|os.O_NONBLOCK)
    proc=spawn([simulator,'--headless'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,
               stderr=subprocess.STDOUT,env={**os.environ,'SDL_VIDEODRIVER':'dummy','PROTOMATE_HMI_AUTOMATION':str(fifo)})
    selector=selectors.DefaultSelector();selector.register(proc.stdout,selectors.EVENT_READ)
    pending=b'';transcript=bytearray()
    def prompt(timeout=30):
        nonlocal pending
        end=time.monotonic()+timeout
        while b'> ' not in pending:
            assert time.monotonic()<end,transcript.decode(errors='replace')[-5000:]
            for key,_ in selector.select(.1):
                block=os.read(key.fileobj.fileno(),16384)
                assert block,f'Controller exited {proc.poll()}'
                transcript.extend(block);pending+=block;(tmp_path/'console.log').write_bytes(transcript)
        reply,pending=pending.split(b'> ',1);return reply.decode(errors='replace')
    def command(text):
        proc.stdin.write((text+'\n').encode());proc.stdin.flush();return prompt()
    def input(text):os.write(pipe,(text+'\n').encode());time.sleep(.12)
    def click(x,y):input(f'down {x} {y}');input(f'up {x} {y}')
    def number(x,y,value):
        click(x,y);click(750,238) # remove initial zero, not an execution action
        for c in str(value):
            if c=='.':click(532,430)
            elif c=='-':click(260,430)
            elif c=='0':click(396,430)
            else:
                digit=int(c)-1;click(260+(digit%3)*136,238+(digit//3)*64)
        click(750,430)
    def capture(name):
        # Wait for telemetry and the independently paced full-detail image.
        time.sleep(.8)
        p=tmp_path/(name+'.bmp');input(f'capture {p}');assert p.exists() and p.stat().st_size>100000
        return json.loads(p.with_suffix('.bmp.json').read_text())
    def wait_controls(expected):
        # IO acknowledgement can precede the UI's 20 Hz presentation snapshot.
        # Clicking a still-disabled control must not enqueue a future motion.
        end=time.monotonic()+5
        for attempt in range(100):
            path=tmp_path/f'controls-{attempt}.bmp'
            input(f'capture {path}')
            metadata=path.with_suffix('.bmp.json')
            if metadata.exists() and json.loads(metadata.read_text())['controls']==expected:
                return
            assert time.monotonic()<end,'GUI controls did not receive authoritative readiness'
        raise AssertionError('GUI readiness capture limit exceeded')
    def compare_presented_state(snapshot):
        out=command('sim status')
        actual=[float(v) for v in re.findall(r'joint=([-+\d.eE]+)',out)]
        tcp=[float(v) for v in re.findall(r'TCP [XYZ]=([-+\d.eE]+)',out)]
        assert len(actual)==len(tcp)==3,out
        actual[2]/=1000 # native CLI reports Z in mm; HMI model uses SI
        assert snapshot['flags']==7,snapshot
        assert snapshot['encoder_mask']==1,snapshot
        # The native fusion-cad-v3 plant uses 5:1 rotary reductions and a
        # 1.25 mm lead. Commanded endpoints agree within reference/grid rounding.
        for shown,real,tolerance in zip(snapshot['pose'][:3],actual,(.0004,.0004,.000002)):
            assert abs(shown-real)<tolerance,(snapshot,actual)
        for shown,real in zip(snapshot['pose'][6:9],tcp):
            assert abs(shown-real)<.25,(snapshot,tcp)
        # Compare the displayed feedback to the actual client telemetry. Using
        # displacements cancels the calibrated reference/grid offsets.
        motors=command('motor status all')
        commanded=[float(v) for v in re.findall(r'm\d: position=([-+\d.eE]+)',motors)]
        feedback=[float(v) for v in re.findall(r'(?:shaft encoder|driver INDEX \(pseudo\)): position=([-+\d.eE]+)',motors)]
        assert len(commanded)==len(feedback)==3,motors
        for i,scale in enumerate((math.pi/180/5,math.pi/180/5,.00125/360)):
            delta=snapshot['pose'][3+i]-snapshot['pose'][i]
            assert abs(delta-(feedback[i]-commanded[i])*scale)<abs(scale)*.0002,motors
        # INDEX is a four-full-step electrical-cycle estimate, NOT an encoder.
        # Its uncertainty is 7.2 shaft degrees, regardless of microstepping.
        resolutions=(2*math.pi/1600/5,math.radians(7.2)/5,.00125/50)
        for shown,real,resolution,rounding in zip(snapshot['pose'][3:6],actual,resolutions,(.0004,.0004,.000002)):
            assert abs(shown-real)<resolution+rounding,(snapshot,actual)
        # Independently evaluate the mixed-feedback TCP, including the CAD tool
        # offset; do not label it measured or demand sub-INDEX plant accuracy.
        q1,q2,z=snapshot['pose'][3:6]
        estimated=(205*math.cos(q1)+358.4*math.cos(q1+q2),
                   205*math.sin(q1)+358.4*math.sin(q1+q2),28.9+z*1000)
        for shown,expected in zip(snapshot['pose'][9:12],estimated):
            assert abs(shown-expected)<.001,(snapshot,estimated)
    def latest():
        output=command('motion jobs');ids=re.findall(r'^(\d+):',output,re.M);assert ids,output
        return max(map(int,ids))
    def wait(job,states=('completed',),timeout=90):
        end=time.monotonic()+timeout
        while time.monotonic()<end:
            out=command(f'motion jobs {job}')
            line=next((x for x in out.splitlines() if x.startswith(f'{job}: ')),out)
            if any(s in line for s in states):return line
            assert not any(s in line for s in ('rejected','faulted','unknown;')),line
            time.sleep(.1)
        raise AssertionError(out)
    def joints():
        out=command('sim status');rows=re.findall(r'm(\d) shaft=([-+\d.eE]+) deg; joint=([-+\d.eE]+)',out)
        assert len(rows)==3,out;return [float(j) for _,_,j in rows]
    try:
        prompt();time.sleep(1);capture('home-disabled')
        click(880,490) # Initialize through GUI
        end=time.monotonic()+5
        while 'ready=1' not in command('motion status'):
            assert time.monotonic()<end,'GUI initialization did not reach IO'
            time.sleep(.1)
        assert 'enabled=0' in command('motion status')
        wait_controls(13) # initialized, connected and fresh; Enable is now actionable
        click(880,490)
        end=time.monotonic()+5
        while 'enabled=1' not in command('motion status'):
            assert time.monotonic()<end,'GUI enable did not reach IO'
            time.sleep(.1)
        time.sleep(.5);click(880,490);interrupted=latest();wait(interrupted,('running',));click(940,560);wait(interrupted,('stopped',))
        click(880,490);home=latest();assert home>interrupted;wait(home)
        compare_presented_state(capture('home-referenced'));before_robot=joints()
        click(50,154);click(890,118);click(740,250)
        number(240,264,-.5);number(520,264,-.3);number(800,264,-.1)
        click(820,488);robot_job=latest();wait(robot_job)
        after_robot=joints()
        assert abs(math.degrees(after_robot[0]-before_robot[0])+.5)<.05,(before_robot,after_robot)
        assert abs(math.degrees(after_robot[1]-before_robot[1])+.3)<.05,(before_robot,after_robot)
        assert abs((after_robot[2]-before_robot[2])+.1)<.01,(before_robot,after_robot)
        compare_presented_state(capture('robot-motion'))
        before=joints()
        click(50,154);click(540,246);click(572,330);step=latest();wait(step)
        after=joints();assert abs(math.degrees(after[0]-before[0])+1)<.05,(before,after)
        capture('axis')
        click(912,246) # Hold mode
        input('down 572 330');jog=latest();wait(jog,('running',))
        input('focuslost');wait(jog,('stopped',));capture('jog-released');input('focusgained')
        # Cartesian Hold repeats only completed increments and stops on release.
        click(660,118)
        before_cart=joints();previous=latest()
        input('down 220 448') # Z minus, one mm per completed point move
        first=latest();assert first>previous;wait(first)
        end=time.monotonic()+12
        while latest()==first:
            assert time.monotonic()<end,'Held Cartesian increment did not repeat'
            time.sleep(.1)
        second=latest();input('up 220 448');wait(second,('stopped','completed'))
        after_release=latest();time.sleep(.6);assert latest()==after_release
        assert joints()[2]<before_cart[2]-.9,(before_cart,joints())
        capture('cartesian-held')
        # A CLI stop ends the held sequence too; do not retry while still pressed.
        input('down 220 448');stopped=latest();wait(stopped,('running',))
        command('motion stop');wait(stopped,('stopped',))
        time.sleep(.6);assert latest()==stopped
        input('up 220 448');click(50,154)
        # The same IO admission still excludes the GUI's axis from a raw CLI job.
        raw=command('motor move m2 -2 --speed 1');raw_id=int(re.search(r'Submitted motor job (\d+)',raw)[1]);wait(raw_id,('running','accepted'))
        click(540,246);click(572,330);rejected=latest();assert rejected>raw_id
        assert 'rejected' in wait(rejected,('rejected',))
        command('motor stop all');wait(raw_id,('stopped',))
        # Empty pages and numeric cancellation produce no new job.
        last=latest();click(50,250);capture('programs-empty');assert latest()==last
        click(50,154);click(720,182);click(650,320);capture('keypad');click(750,365);assert latest()==last
        click(50,346);capture('io');click(50,538);capture('settings');click(50,442);capture('alarms')
        input('size 1600 600');click(338,58);capture('letterbox-home')
        input('quit');assert proc.wait(10)==0,transcript.decode(errors='replace')[-2000:]
    finally:
        os.close(pipe);selector.close()
