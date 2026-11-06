#!/usr/bin/env python3
"""Panel-free HMI endurance check; defaults to the selected 2 fps CAD cadence.

The old technical showcase's continuous 60 fps assertion does not apply to the
normal, dirty-region HMI. Use the same memory/ICC/error checks as its short screen.
"""
import sys
from hmi_hardware_check import main

if __name__ == '__main__':
    if '--seconds' not in sys.argv:
        sys.argv.extend(['--seconds', '600'])
    if '--rates' not in sys.argv:
        sys.argv.extend(['--rates', '2'])
    main()
