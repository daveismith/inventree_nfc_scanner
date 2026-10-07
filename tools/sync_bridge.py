#!/usr/bin/env python3
"""Make a USB scanner look like a network scanner to the InvenTree plugin.

Connects to the scanner over serial and speaks the plugin's `/sync` exchange on its behalf,
as the firmware's own network link does: events from the scanner go up, numbered; commands
come down, numbered, and are acknowledged once acted on. It lets the plugin's network route
be exercised with a scanner that has no radio, or whose radio is off, and is the reference
client the firmware's `net_sync` follows. Commands the firmware refuses from its network link
(`bootloader`, `debug`, `net`, `hid`) are refused here too.

    sync_bridge.py --url http://inventree.localhost:8080/plugin/nfcscanner --token <api token>

The reader id defaults to the scanner's own (from its USB serial number, which is its MAC),
and must match a machine configured in InvenTree. The token must belong to that machine's
user. `--wait` sets the long-poll hold the plugin is asked for (0: plain polling).
"""

import argparse
import json
import random
import sys
import time
import urllib.error
import urllib.request

sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from nfcprog import APP_PID, ESPRESSIF_VID, Device, redacted  # noqa: E402

POLL_MIN_S = 1.0        # never call more often than this when idle, whatever the server does
BACKOFF_MAX_S = 30.0


def find_scanner():
    import serial.tools.list_ports

    for p in serial.tools.list_ports.comports():
        if p.vid == ESPRESSIF_VID and p.pid == APP_PID:
            return p.device, (p.serial_number or '').lower()
    sys.exit(f'no scanner found at {ESPRESSIF_VID:04x}:{APP_PID:04x}')


class Bridge:
    def __init__(self, dev, url, token, reader, wait_s, verbose):
        self.dev = dev
        self.url = url.rstrip('/') + '/sync/'
        self.token = token
        self.reader = reader
        self.wait_s = wait_s
        self.verbose = verbose
        self.boot = random.randint(1, 1_000_000)   # a new numbering for the server, like a restart
        self.seq = 0
        self.unacked = []           # messages sent and not yet acknowledged, oldest first
        self.acted = set()          # command seqs already acted on
        self.highest_cmd = 0
        self.backoff = 1.0

    def log(self, *a):
        print(time.strftime('%H:%M:%S'), *a, flush=True)

    def queue(self, msg):
        self.seq += 1
        self.unacked.append(dict(msg, seq=self.seq))

    def collect(self, seconds):
        """Gather what the scanner says for a while; rsp and evt lines are for the server."""
        for msg in self.dev.lines(seconds):
            if 'rsp' in msg or 'evt' in msg:
                if msg.get('evt') == 'log':
                    continue
                self.queue(msg)
                if self.verbose:
                    self.log('scanner:', json.dumps(msg, separators=(',', ':')))

    def sync(self):
        body = {
            'reader': self.reader, 'boot': self.boot, 'proto': 1, 'ack': self.highest_cmd,
            'wait_s': self.wait_s, 'msgs': list(self.unacked),
        }
        req = urllib.request.Request(
            self.url, method='POST', data=json.dumps(body).encode(),
            headers={'Authorization': f'Token {self.token}', 'Content-Type': 'application/json'},
        )
        with urllib.request.urlopen(req, timeout=self.wait_s + 15) as r:
            try:
                return json.loads(r.read())
            except ValueError as e:
                raise urllib.error.URLError(f'the server answered with something that is not JSON ({e})')

    # The firmware refuses these from its own network link; over USB it cannot tell, so the
    # bridge refuses them on its behalf and answers as the firmware would.
    NOT_FROM_THE_NETWORK = {'bootloader', 'debug', 'net', 'hid'}

    def act(self, cmd):
        """Hand a command to the scanner. Its rsp and events come back through collect()."""
        payload = {k: v for k, v in cmd.items() if k != 'seq'}
        self.log('command:', json.dumps(redacted(payload), separators=(',', ':'))[:120])
        if payload.get('cmd') in self.NOT_FROM_THE_NETWORK:
            rsp = {'rsp': payload['cmd'], 'ok': False, 'error': 'not_allowed'}
            if 'id' in payload:
                rsp['id'] = payload['id']
            self.queue(rsp)
            return
        self.dev.send(payload)

    def run(self):
        self.log(f'bridging {self.reader} to {self.url} (boot {self.boot}, wait {self.wait_s}s)')
        while True:
            started = time.monotonic()
            self.collect(0.2)
            try:
                reply = self.sync()
            except urllib.error.HTTPError as e:
                detail = e.read().decode(errors='replace')[:200]
                if e.code in (401, 403, 404):
                    self.log(f'the server refuses this scanner ({e.code}: {detail}); retrying in 60 s')
                    self.collect(60)
                    continue
                self.log(f'server error {e.code}: {detail}; retrying in {self.backoff:.0f}s')
                self.collect(self.backoff)
                self.backoff = min(self.backoff * 2, BACKOFF_MAX_S)
                continue
            except (urllib.error.URLError, OSError) as e:
                self.log(f'cannot reach the server ({e}); retrying in {self.backoff:.0f}s')
                self.collect(self.backoff)
                self.backoff = min(self.backoff * 2, BACKOFF_MAX_S)
                continue
            self.backoff = 1.0

            ack = reply.get('ack', 0)
            self.unacked = [m for m in self.unacked if m['seq'] > ack]
            for cmd in reply.get('cmds', []):
                seq = cmd.get('seq', 0)
                if seq in self.acted:
                    continue
                self.acted.add(seq)
                self.highest_cmd = max(self.highest_cmd, seq)
                self.act(cmd)

            # Plain polling: pace ourselves. Long polling: the server paced us.
            idle = max(0.0, POLL_MIN_S - (time.monotonic() - started))
            poll_ms = reply.get('poll_ms')
            if poll_ms:
                idle = max(idle, poll_ms / 1000 - (time.monotonic() - started))
            self.collect(idle)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--url', required=True, help="the plugin's URL, e.g. https://host/plugin/nfcscanner")
    ap.add_argument('--token', required=True, help="an InvenTree API token of the scanner machine's user")
    ap.add_argument('--reader', help='reader id to present (default: nfc-<mac> from the scanner)')
    ap.add_argument('--port', help='serial port; found by USB ID when omitted')
    ap.add_argument('--wait', type=int, default=25, help='long-poll hold to ask for, seconds (0: plain polling)')
    ap.add_argument('-v', '--verbose', action='store_true')
    args = ap.parse_args()

    port, serial_number = (args.port, '') if args.port else find_scanner()
    reader = args.reader or (f'nfc-{serial_number}' if serial_number else None)
    if not reader:
        sys.exit('pass --reader: the port gives no serial number to derive it from')

    dev = Device(port, args.verbose)
    try:
        info = dev.request({'cmd': 'info'})
        print('scanner:', json.dumps({k: info.get(k) for k in ('fw', 'pn532', 'state', 'hid')}), flush=True)
        dev.request({'cmd': 'hid', 'enabled': False}).get('ok')   # the plugin drives it now
        Bridge(dev, args.url, args.token, reader, args.wait, args.verbose).run()
    except KeyboardInterrupt:
        pass
    finally:
        dev.close()


if __name__ == '__main__':
    main()
