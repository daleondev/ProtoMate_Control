#!/usr/bin/env python3
"""Audit linked M4 IRQ call paths and known indirect targets for RAM residency.

This is a build guard, not a general C++ call-graph proof. The explicit callback
roots describe the production motor composition; new callback types require a
review here. The JSON report exposes every indirect call site for that review.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('elf',type=Path)
parser.add_argument('--report',type=Path)
args=parser.parse_args()
s=subprocess.check_output(['arm-none-eabi-objdump','-d',str(args.elf)],text=True)
funcs={};owner=None;literal_edges=[]
for l in s.splitlines():
 m=re.match(r'^([0-9a-f]+) <(.+)>:',l)
 if m:owner=m[2];funcs[owner]={'address':int(m[1],16),'calls':set(),'indirect':[]};continue
 if not owner:continue
 if 'veneer' in owner:
  word=re.search(r'\.word\s+0x([0-9a-f]+)',l)
  if word:literal_edges.append((owner,int(word[1],16)&~1))
 m=re.search(r'\b(?:bl|b\.w|b\.n|b|blx)\s+([0-9a-f]+) <([^>]+)>',l)
 if m:funcs[owner]['calls'].add(m[2].split('+')[0])
 if re.search(r'\bblx\s+r|\bbx\s+r(?:[0-9]|1[0-2])\b',l):funcs[owner]['indirect'].append(l.strip())
by_address={f['address']:n for n,f in funcs.items()}
for owner,address in literal_edges:
 if address in by_address:funcs[owner]['calls'].add(by_address[address])
names=list(funcs)
demangled=subprocess.check_output(['arm-none-eabi-c++filt'],input='\n'.join(names),text=True).splitlines()
labels=dict(zip(names,demangled))
roots=[]
for n,label in labels.items():
 if n in ('HardFault_Handler','MemManage_Handler','BusFault_Handler','UsageFault_Handler'):roots.append(n)
 if 'thread_stack_error_handler(' in label or label == 'std::chrono::_V2::steady_clock::now()':roots.append(n)
 if re.match(r'^(DMA1_Stream[0-3]|TIM[3567](?:_DAC)?|EXTI[0-9_]+)_IRQHandler$',n):roots.append(n)
 if '_S_call_storage' in label and '_Base_invoker<true' in label and any(c in label for c in ['StepperMotor::StepperMotor','Controller::Controller','IndexFeedback::IndexFeedback','QuadratureEncoderFeedback::subscribe','DriverFaultMonitor::DriverFaultMonitor']):roots.append(n)
 if (any(x in label for x in ['::Sequence::generate(', '::Sequence::timing(', '::Sequence::count(', '::Sequence::maximumPeriod(', '::accountProgress(', '::accountFeedback(', '::IndexFeedback::motion(', '::IndexFeedback::invalidate(', '::QuadratureEncoderFeedback::motion(', '::QuadratureEncoderFeedback::invalidate(', '::GpioOutput::write(', '::GpioInput::read(']) and '~' not in label and 'veneer' not in label):roots.append(n)
 if label.startswith('hal::stm32::StepHardware::') and any('::'+x+'(' in label for x in ['buffers','sample','guard','completionWatch','acknowledge','finishAxis','stopAxis','stop','publish','pin_mode']):roots.append(n)
 if any(x in label for x in ['hal::device::(anonymous namespace)::now(', 'hal::(anonymous namespace)::now(', 'runtime::thread::fatal_error(']):roots.append(n)
seen=set();todo=list(roots);outside=[]
while todo:
 n=todo.pop()
 if n in seen or n not in funcs:continue
 seen.add(n);f=funcs[n]
 if not 0x10000000 <= f['address'] < 0x10020000:outside.append((n,f['address']))
 todo.extend(f['calls'])

# Virtual dispatch reads these tables before entering the concrete RAM methods.
symbols=subprocess.check_output(['arm-none-eabi-nm','-nC',str(args.elf)],text=True)
vtable_classes=('hal::stm32::StepHardware','hal::GpioInput','hal::GpioOutput',
                'hal::device::IndexFeedback','hal::device::QuadratureEncoderFeedback',
                'io::motion::Sequence')
vtables={}
for line in symbols.splitlines():
 for cls in vtable_classes:
  if line.endswith('vtable for '+cls):
   address=int(line.split()[0],16);vtables[cls]=address
   if not 0x10000000 <= address < 0x10020000:outside.append(('vtable for '+cls,address))
for required in ['DMA1_Stream0_IRQHandler','DMA1_Stream3_IRQHandler','TIM7_IRQHandler',
                 'TIM3_IRQHandler','TIM6_DAC_IRQHandler','TIM5_IRQHandler','HardFault_Handler']:
 if required not in roots:raise SystemExit('Missing mandatory interrupt root: '+required)
report=dict(elf=str(args.elf), roots=[labels[n] for n in sorted(set(roots))],
            functions=len(seen), outside_ram=[dict(symbol=labels.get(n,n),address=hex(a)) for n,a in outside],
            vtables={n:hex(a) for n,a in vtables.items()},
            indirect_calls={labels[n]:funcs[n]['indirect'] for n in sorted(seen) if funcs[n]['indirect']})
if args.report:args.report.write_text(json.dumps(report,indent=2)+'\n')
for n,a in outside:print(f'{a:#010x}: {labels.get(n,n)}')
print(f'M4 RAM audit: {len(set(roots))} roots, {len(seen)} reachable functions, {len(vtables)} vtables; {len(outside)} outside RAM')
raise SystemExit(bool(outside))
