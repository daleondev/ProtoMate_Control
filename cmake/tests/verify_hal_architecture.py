#!/usr/bin/env python3
"""Check the HAL layer, naming and CMake ownership rules documented in README.md."""
import argparse
import json
from pathlib import Path
import re
import sys

INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]', re.MULTILINE)
SYNCHRONIZATION = {
    'hal/stm32/InterruptGuard.hpp',
    'hal/linux/Mutex.hpp',
    'hal/linux/ThreadMutex.hpp',
}
PORTABLE = ('hal/drivers/itf/', 'hal/devices/itf/', 'hal/util/')


def include_error(source, target):
    """Names are relative to platform/, independent of the current build."""
    if not source.startswith('hal/'):
        return None
    if target.startswith(('control/', 'cli/', 'runtime/')):
        return 'HAL must not depend on application or runtime implementation headers'
    if '/itf/' in source or ('/factory/' in source and source.endswith('.hpp')) or source == 'hal/board/board.hpp':
        allowed = PORTABLE if not source.startswith('hal/drivers/') else ('hal/drivers/itf/', 'hal/util/')
        if target.startswith('hal/') and not target.startswith(allowed):
            return 'public contracts/factory declarations may include only contracts and common util'
        if target.startswith(('stm32', 'tx_', 'fx_', 'lx_', 'linux/', 'sys/', 'pthread')):
            return 'public contracts/factory declarations must be platform independent'
    if source.startswith('hal/drivers/'):
        if target.startswith(('hal/devices/', 'hal/board/')):
            return 'drivers must not depend on devices or board wiring'
        if ('/impl/' in source or '/util/' in source) and '/factory/' in target:
            return 'implementations/util must not depend on factories'
    if source.startswith('hal/devices/'):
        if target.startswith('hal/board/'):
            return 'devices must not depend on board wiring'
        if '/factory/' not in source:
            if target.startswith(('hal/drivers/impl/', 'hal/drivers/util/', 'hal/drivers/factory/', 'hal/devices/factory/')):
                return 'device behavior must use driver contracts, not concrete drivers or factories'
            if target.startswith(('hal/linux/', 'hal/stm32/')) and target not in SYNCHRONIZATION:
                return 'common devices may use platform synchronization, not register/platform APIs'
            if target == 'hal/hal.hpp':
                return 'device behavior must not access bootstrap/vendor HAL'
    if source.startswith('hal/board/'):
        if target.startswith(('hal/drivers/impl/', 'hal/drivers/util/', 'hal/devices/impl/linux/', 'hal/devices/impl/stm32/')):
            return 'board wiring must use factories and portable device implementations'
    if '/stm32/' in source and ('/linux/' in target or target.startswith('linux/')):
        return 'STM32 implementation must not include Linux implementation'
    if '/linux/' in source and '/stm32/' in target:
        return 'Linux implementation must not include STM32 implementation'
    return None


def check_sources(root):
    platform = root / 'platform'
    hal = platform / 'hal'
    errors = []
    checked = 0
    for path in sorted(hal.rglob('*')):
        if 'tests' in path.relative_to(hal).parts:
            continue
        if path.is_dir() and path.name in ('detail', 'utilities'):
            errors.append(f'{path.relative_to(root)}: helper directories must be named util')
        if not path.is_file() or path.suffix not in ('.cpp', '.hpp', '.h'):
            continue
        checked += 1
        source = path.relative_to(platform).as_posix()
        text = path.read_text()
        if re.search(r'\bnamespace\s+(?:\w+::)*(?:detail|utilities)\b|\b(?:detail|utilities)::', text):
            errors.append(f'{source}: helper namespaces must be named util')
        if '/impl/' in source and path.suffix == '.cpp' and not path.with_suffix('.hpp').is_file():
            errors.append(f'{source}: implementation must have a matching declaration header')
        if '/itf/' in source and path.stem.startswith('I') and not re.search(r'\bclass\s+' + path.stem + r'\b', text):
            errors.append(f'{source}: I-prefixed contract headers must declare their interface')
        targets = []
        for match in INCLUDE.finditer(text):
            target = match.group(1)
            local = (path.parent / target).resolve()
            if local.is_file() and local.is_relative_to(platform):
                target = local.relative_to(platform).as_posix()
            if '/detail/' in target or '/utilities/' in target:
                errors.append(f'{source}: obsolete helper path {target}')
            if target.startswith('hal/') and not (platform / target).is_file():
                errors.append(f'{source}: missing HAL header {target}')
            problem = include_error(source, target)
            if problem:
                line = text.count('\n', 0, match.start()) + 1
                errors.append(f'{source}:{line}: {problem}: {target}')
            targets.append(target)
        if '/factory/' in source and path.suffix == '.cpp':
            layer = source.split('/factory/')[0]
            header = f'{layer}/factory/{path.stem}.hpp'
            if header not in targets:
                errors.append(f'{source}: factory implementation must include {header}')
    return checked, errors


def check_build(root, build, platform_name):
    errors = []
    compiled = set()
    forbidden = 'linux' if platform_name == 'stm32' else 'stm32'
    for entry in json.loads((build / 'compile_commands.json').read_text()):
        path = Path(entry['file']).resolve()
        if not path.is_relative_to(root / 'platform' / 'hal'):
            continue
        source = path.relative_to(root / 'platform').as_posix()
        if '/tests/' in source:
            continue
        compiled.add(source)
        if source.startswith('hal/drivers/'):
            owners = ('hal_drivers',)
            if source in ('hal/drivers/impl/linux/StepHardware.cpp', 'hal/drivers/impl/linux/QuadratureEncoder.cpp'):
                owners = ('hal_linux_services',)
        elif source.startswith('hal/devices/'):
            owners = ('hal_devices',)
        elif source.startswith('hal/board/'):
            owners = ('hal_board',)
        else:
            owners = ('hal',)
        command = entry.get('output', entry.get('command', ''))
        if not any(f'CMakeFiles/{owner}.dir/' in command for owner in owners):
            errors.append(f'{source}: source must be compiled by {owners}')
        if f'/{forbidden}/' in source:
            errors.append(f'{source}: wrong platform selected for {platform_name}')
    for path in (root / 'platform' / 'hal').rglob('*.cpp'):
        parts = path.relative_to(root / 'platform' / 'hal').parts
        if 'tests' in parts or forbidden in parts:
            continue
        source = path.relative_to(root / 'platform').as_posix()
        if source not in compiled:
            errors.append(f'{source}: selected production source is missing from the build')
    for layer in ('drivers', 'devices'):
        for header in (root / 'platform' / 'hal' / layer / 'factory').glob('*.hpp'):
            candidates = {
                f'hal/{layer}/factory/{header.stem}.cpp',
                f'hal/{layer}/factory/{platform_name}/{header.stem}.cpp',
            }
            if len(compiled & candidates) != 1:
                errors.append(f'{header.relative_to(root)}: expected exactly one selected factory implementation')
    return errors


def self_test():
    # Prove the guard rejects representative forbidden edges and accepts the
    # deliberate construction and synchronization boundaries.
    forbidden = [
        ('hal/drivers/itf/ISpi.hpp', 'hal/stm32/hal.hpp'),
        ('hal/drivers/impl/linux/Uart.cpp', 'hal/devices/impl/Tmc2209.hpp'),
        ('hal/drivers/impl/stm32/Spi.cpp', 'hal/drivers/factory/spi.hpp'),
        ('hal/devices/impl/IndexFeedback.cpp', 'hal/drivers/impl/linux/Gpio.hpp'),
        ('hal/board/nucleo_h753zi/board.cpp', 'hal/devices/impl/linux/Tmc2209Model.hpp'),
        ('hal/devices/impl/Tmc2209.cpp', 'control/StepperMotor.hpp'),
    ]
    allowed = [
        ('hal/drivers/impl/stm32/Spi.cpp', 'hal/drivers/itf/ISpi.hpp'),
        ('hal/devices/factory/linux/tmc2209.cpp', 'hal/drivers/impl/linux/Uart.hpp'),
        ('hal/devices/impl/IndexFeedback.cpp', 'hal/stm32/InterruptGuard.hpp'),
        ('hal/board/nucleo_h753zi/board.cpp', 'hal/devices/factory/tmc2209.hpp'),
    ]
    assert all(include_error(*edge) for edge in forbidden)
    assert not any(include_error(*edge) for edge in allowed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--build', type=Path)
    parser.add_argument('--platform', choices=('linux', 'stm32'), default='linux')
    args = parser.parse_args()
    self_test()
    root = args.source.resolve()
    count, errors = check_sources(root)
    if args.build:
        errors += check_build(root, args.build.resolve(), args.platform)
    if errors:
        print('\n'.join(errors), file=sys.stderr)
        return 1
    print(f'HAL architecture: {count} production files checked; layer, naming and factory rules passed'
          + ('; build source ownership passed' if args.build else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
