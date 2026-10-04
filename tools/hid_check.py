#!/usr/bin/env python3
"""Check the scanner's keyboard output, safely.

Turns keyboard output on for this session only, asks for a tag to be tapped, and reads what
the scanner types straight back from this terminal, so the keystrokes land somewhere that
expects them and are compared with what the tag holds:

    python tools/hid_check.py

Keep this terminal window focused while you tap. When the script ends the port closes, and
the scanner goes back to its stored setting.
"""

import argparse
import os
import select
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nfcprog import Device, find_port  # noqa: E402

WAIT_S = 90


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', help='serial port; found by USB ID when omitted')
    args = ap.parse_args()

    dev = Device(args.port or find_port())
    try:
        info = dev.request({'cmd': 'info'})
        if info.get('tag'):
            print('Take the tag off the reader first, then run this again.')
            return 1
        rsp = dev.request({'cmd': 'hid', 'enabled': True})
        if not rsp.get('enabled'):
            print(f'the scanner would not turn keyboard output on: {rsp}')
            return 1

        print('Keyboard output is on for this session.')
        print('\n>> Keep THIS window focused and tap a tag that has a text record.\n')
        tag = None
        for msg in dev.lines(WAIT_S):
            if msg.get('evt') == 'tag':
                tag = msg
                break
        if tag is None:
            print('no tag was presented')
            return 1
        text = tag.get('text')
        if not text:
            print(f'that tag has no text record, so nothing is typed: {tag}')
            return 1

        # The scanner types the text and Enter, which the terminal hands over as one line.
        ready, _, _ = select.select([sys.stdin], [], [], 5)
        if not ready:
            print(f'\nFAIL: the tag holds "{text}" but nothing was typed here.')
            print('      Was this window focused? Does this firmware have the keyboard interface?')
            return 1
        typed = sys.stdin.readline().rstrip('\n')
        if typed == text:
            print(f'\npass: the scanner typed "{typed}" and Enter')
            print('To make typing the default: python tools/nfcprog.py hid on --persist')
            return 0
        print(f'\nFAIL: the tag holds "{text}" but "{typed}" arrived.')
        print('      A mismatch in punctuation usually means the host keyboard layout is not US.')
        return 1
    finally:
        dev.close()


if __name__ == '__main__':
    sys.exit(main())
