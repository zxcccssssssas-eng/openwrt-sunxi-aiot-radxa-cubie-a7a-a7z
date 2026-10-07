#!/usr/bin/env python3
"""Exercise the prepared rtw88 USB RX functions with injected USB/allocation errors.

Usage: python3 rtw88-usb-rx-recovery.py /path/to/backports/drivers/net/wireless/realtek/rtw88/usb.c
Requires a host C compiler; uses AddressSanitizer and UndefinedBehaviorSanitizer.
The shims model ownership and work scheduling, not kernel concurrency or hardware.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('source', type=Path, help='prepared rtw88/usb.c')
source = parser.parse_args().source.read_text()
start = source.index('static void rtw_usb_read_port_complete(struct urb *urb);')
end = source.index('static void rtw_usb_free_rx_bufs', start)
with tempfile.TemporaryDirectory(prefix='rtw88-rx-test-') as directory:
    directory = Path(directory)
    (directory / 'rtw88-usb-rx-functions.h').write_text(source[start:end])
    test = Path(__file__).with_suffix('.c')
    binary = directory / 'test'
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                    '-Werror', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-g', '-I', str(directory), str(test), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
