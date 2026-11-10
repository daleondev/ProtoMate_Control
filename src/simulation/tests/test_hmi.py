"""Real SDL input -> TouchGFX -> UI client -> ICC -> IO -> native plant."""
import math
import json
import os
import re
import selectors
import subprocess
import time


def test_hmi_motion_workflow(simulator,tmp_path,spawn,wall_time_factor):
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
        # The native fusion-cad-v4 plant uses 5:1 rotary reductions and a
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
        end=time.monotonic()+timeout*wall_time_factor
        while time.monotonic()<end:
            out=command(f'motion jobs {job}')
            line=next((x for x in out.splitlines() if x.startswith(f'{job}: ')),out)
            if any(s in line for s in states):return line
            assert not any(s in line for s in ('rejected','faulted','unknown;')),line
            time.sleep(.1)
        raise AssertionError(out)
    def simulation_time():
        return float(re.search(r'time=([\d.]+) s',command('sim status'))[1])
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
        # Include final switch clearance and the full sequential neutral return.
        click(880,490);home=latest();assert home>interrupted;wait(home,timeout=180)
        compare_presented_state(capture('home-referenced'));before_robot=joints()
        click(50,154);click(890,118);click(740,250)
        number(240,264,-4);number(520,264,-4);number(800,264,-.1)
        click(820,488);robot_job=latest();wait(robot_job)
        after_robot=joints()
        assert abs(math.degrees(after_robot[0]-before_robot[0])+4)<.05,(before_robot,after_robot)
        assert abs(math.degrees(after_robot[1]-before_robot[1])+4)<.05,(before_robot,after_robot)
        assert abs((after_robot[2]-before_robot[2])+.1)<.01,(before_robot,after_robot)
        compare_presented_state(capture('robot-motion'))
        before=joints()
        click(50,154);click(540,246);click(572,330);step=latest();wait(step)
        after=joints();assert abs(math.degrees(after[0]-before[0])+1)<.05,(before,after)
        capture('axis')
        click(912,246) # Hold mode
        input('down 572 330');jog=latest();wait(jog,('running',))
        input('focuslost');wait(jog,('stopped',));capture('jog-released');input('focusgained')
        # Cartesian Hold is ONE continuous path, stopped locally on release.
        click(660,118)
        before_cart=joints();previous=latest()
        input('down 516 468') # Z minus
        first=latest();assert first>previous;wait(first,('running',))
        started=simulation_time();end=time.monotonic()+15
        while simulation_time()<started+1.5:
            assert time.monotonic()<end,'Cartesian hold did not advance'
            assert latest()==first,'Continuous jog created another point-move job'
            time.sleep(.1)
        input('up 516 468');wait(first,('stopped',))
        after_release=latest();time.sleep(.6);assert latest()==after_release
        assert joints()[2]<before_cart[2]-.1,(before_cart,joints())
        capture('cartesian-held')
        # X/Y need changing joint ratios. Observe native plant TCP during a
        # single held job, independently of UI feedback/INDEX estimates.
        for x,y,selected in ((644,324,0),(516,396,1)):
            out=command('sim status');origin=[float(v) for v in re.findall(r'TCP [XYZ]=([-+\d.eE]+)',out)]
            input(f'down {x} {y}');cart=latest();wait(cart,('running',))
            start_time=simulation_time();end=time.monotonic()+15
            while simulation_time()<start_time+.7:
                assert time.monotonic()<end
                assert latest()==cart
                values=[float(v) for v in re.findall(r'TCP [XYZ]=([-+\d.eE]+)',command('sim status'))]
                for i in range(3):
                    if i!=selected: assert abs(values[i]-origin[i])<.2,(origin,values)
            input(f'up {x} {y}');wait(cart,('stopped',))
        # A CLI stop ends the held job too; do not retry while still pressed.
        input('down 516 468');stopped=latest();wait(stopped,('running',))
        command('motion stop');wait(stopped,('stopped',))
        time.sleep(.6);assert latest()==stopped
        input('up 516 468')
        # Step is a single selected displacement; Timed continues after release.
        click(540,246);click(860,348)
        before_step=joints();click(516,468);step=latest();wait(step)
        assert abs(joints()[2]-before_step[2]+.1)<.01
        time.sleep(.3);assert latest()==step
        capture('cartesian-step')
        click(730,246);number(860,416,2)
        before_timed=joints();started=simulation_time();click(516,468);timed=latest()
        wait(timed,('running','completed','stopped'))
        # Controller deadlines use simulation time, which pauses while its
        # ThreadX workers execute; wall-clock sleeps are not a deadline oracle.
        end=time.monotonic()+20
        while simulation_time()<started+2.3:
            assert time.monotonic()<end,'Timed jog did not reach its simulated deadline'
            time.sleep(.1)
        timed_last=latest();assert timed_last==timed;wait(timed_last,('completed','stopped'),timeout=10)
        time.sleep(.6);assert latest()==timed_last
        assert joints()[2]<before_timed[2]-.1
        capture('cartesian-timed')
        # Cartesian Move To uses millimetres and the same authoritative planner.
        click(730,182);number(900,320,-.1)
        before_move=joints();click(620,488);move=latest();wait(move)
        assert abs(joints()[2]-before_move[2]+.1)<.01
        capture('cartesian-moveto')
        click(910,182);capture('cartesian-settings')
        click(50,154)
        # The same IO admission still excludes the GUI's axis from a raw CLI job.
        raw=command('motor move m2 -2 --speed 1');raw_id=int(re.search(r'Submitted motor job (\d+)',raw)[1]);wait(raw_id,('running','accepted'))
        before_busy=joints()
        click(540,246);click(572,330);rejected=latest()
        # Fresh busy telemetry disables the GUI control; if admission raced
        # telemetry, IO must reject it. Neither case may move Axis 1.
        if rejected!=raw_id: assert 'rejected' in wait(rejected,('rejected',))
        assert abs(joints()[0]-before_busy[0])<1e-9
        command('motor stop all');wait(raw_id,('stopped',))
        # Empty pages and numeric cancellation produce no new job.
        last=latest();click(50,250);capture('programs-empty');assert latest()==last
        click(50,154);click(720,182);click(650,320);capture('keypad');click(750,365);assert latest()==last
        click(50,346);capture('io');click(50,538);capture('settings');click(50,442);capture('alarms')
        input('size 1600 600');click(338,58);capture('letterbox-home')
        input('quit');assert proc.wait(10)==0,transcript.decode(errors='replace')[-2000:]
    finally:
        os.close(pipe);selector.close()
