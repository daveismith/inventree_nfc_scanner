#!/usr/bin/env python3
"""Drive the InvenTree NFC scanner over its serial protocol.

This stands in for the InvenTree plugin page: it builds the two-record NDEF message for a
stock location, sends a program job, and prints what the device reports.

    nfcprog.py info
    nfcprog.py program --host inventree.example --pk 42
    nfcprog.py program --host inventree.example --pk 42 --pwd A1B2C3D4 --overwrite
    nfcprog.py wipe --pwd A1B2C3D4
    nfcprog.py monitor
    nfcprog.py hid off
    nfcprog.py log info
    nfcprog.py bootloader
    nfcprog.py ndef --host inventree.example --pk 42      (print the message, send nothing)

The port is found by USB VID:PID unless --port names one (the host simulator's pty, say).
Needs pyserial. The protocol is described in components/proto/include/proto.h.
"""

import argparse
import json
import sys
import time

ESPRESSIF_VID = 0x303A
APP_PID = 0x4E46        # CONFIG_TINYUSB_DESC_CUSTOM_PID in sdkconfig.defaults

# NFC Forum URI record identifier codes for the two schemes a location page can have.
URI_PREFIX = {'https': 0x04, 'http': 0x03}


def ndef_record(type_char, payload, first, last):
    """One short, well-known-type NDEF record."""
    if len(payload) > 255:
        raise ValueError('record payload over 255 bytes; shorten the host name')
    flags = 0x11 | (0x80 if first else 0) | (0x40 if last else 0)      # SR, TNF well-known
    return bytes([flags, 1, len(payload)]) + type_char.encode() + payload


def build_ndef(host, pk, scheme='https'):
    """The message on an InvenTree bin tag.

    A URI record, so a phone opens the location's page, then a Text record holding
    InvenTree's short barcode for the location, which is what the scanner types.
    """
    uri = bytes([URI_PREFIX[scheme]]) + f'{host}/web/stock/location/{pk}'.encode()
    text = b'\x02en' + f'INV-SL{pk}'.encode()      # UTF-8, language "en"
    return ndef_record('U', uri, True, False) + ndef_record('T', text, False, True)


def find_port():
    import serial.tools.list_ports

    ports = [p for p in serial.tools.list_ports.comports() if p.vid == ESPRESSIF_VID and p.pid == APP_PID]
    if not ports:
        sys.exit(f'no scanner found at {ESPRESSIF_VID:04x}:{APP_PID:04x}; plug it in or pass --port')
    return ports[0].device


class Device:
    def __init__(self, port, verbose=False):
        import serial

        self.verbose = verbose
        self.port = serial.Serial(port, 115200, timeout=0.1)
        self.buf = b''

    def close(self):
        self.port.close()

    def send(self, obj):
        line = json.dumps(obj, separators=(',', ':'))
        if self.verbose:
            print(f'> {line}', file=sys.stderr)
        self.port.write(line.encode() + b'\n')
        self.port.flush()

    def lines(self, seconds):
        """Yield each object received in the next `seconds`."""
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.buf += self.port.read(4096)
            while b'\n' in self.buf:
                raw, self.buf = self.buf.split(b'\n', 1)
                raw = raw.strip()
                if not raw:
                    continue
                try:
                    obj = json.loads(raw)
                except ValueError:
                    print(f'! not JSON: {raw!r}', file=sys.stderr)
                    continue
                if self.verbose:
                    print(f'< {raw.decode(errors="replace")}', file=sys.stderr)
                yield obj

    def request(self, obj, seconds=3.0, on_event=None):
        """Send a command and return its `rsp`. Events that arrive meanwhile go to on_event."""
        self.send(obj)
        for msg in self.lines(seconds):
            if msg.get('rsp') == obj['cmd']:
                return msg
            if msg.get('evt') == 'error':
                return msg
            if on_event:
                on_event(msg)
        sys.exit(f'no answer to {obj["cmd"]!r} within {seconds:.0f}s')


def show(msg):
    print(json.dumps(msg, separators=(',', ':')), flush=True)


def run_job(dev, cmd, timeout_s):
    """Send a program or wipe job and follow it to `done` or `failed`. Returns an exit code."""
    rsp = dev.request(cmd, on_event=show)
    show(rsp)
    if not rsp.get('ok'):
        return 1
    for msg in dev.lines(timeout_s + 5):
        show(msg)
        if msg.get('id') != cmd['id']:
            continue
        if msg.get('evt') == 'done':
            return 0
        if msg.get('evt') == 'failed':
            return 1
    print('the job neither finished nor failed', file=sys.stderr)
    return 1


def hex_arg(length):
    def parse(value):
        try:
            raw = bytes.fromhex(value)
        except ValueError:
            raise argparse.ArgumentTypeError('not hex')
        if len(raw) != length:
            raise argparse.ArgumentTypeError(f'expected {length * 2} hex digits')
        return raw.hex().upper()
    return parse


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', help='serial port; found by USB ID when omitted')
    ap.add_argument('-v', '--verbose', action='store_true', help='show every line sent and received')
    sub = ap.add_subparsers(dest='command', required=True)

    sub.add_parser('info', help='firmware, reader and state')

    def add_location(p):
        p.add_argument('--host', required=True, help='InvenTree host name, e.g. inventree.example')
        p.add_argument('--pk', required=True, type=int, help='stock location primary key')
        p.add_argument('--scheme', choices=sorted(URI_PREFIX), default='https')

    p = sub.add_parser('program', help='write a location tag')
    add_location(p)
    p.add_argument('--id', type=int, default=1, help='job id echoed in events')
    p.add_argument('--pwd', type=hex_arg(4), help='password: authenticates, and protects afterwards')
    p.add_argument('--pack', type=hex_arg(2), help='password acknowledge (default 0000)')
    p.add_argument('--old-pwd', type=hex_arg(4), help='current password, when changing it')
    p.add_argument('--overwrite', action='store_true', help='replace a message already on the tag')
    p.add_argument('--timeout', type=float, default=60.0, help='seconds to wait for a tag')

    p = sub.add_parser('wipe', help='empty a tag and remove its password')
    p.add_argument('--id', type=int, default=1)
    p.add_argument('--pwd', type=hex_arg(4))
    p.add_argument('--timeout', type=float, default=60.0)

    sub.add_parser('cancel', help='cancel the waiting job')

    p = sub.add_parser('hid', help='keyboard output on a tap')
    p.add_argument('state', choices=['on', 'off'])
    p.add_argument('--persist', action='store_true', help='make it the stored default')

    p = sub.add_parser('log', help='forward device logs as events, then monitor')
    p.add_argument('level', choices=['off', 'error', 'warn', 'info', 'debug'])

    p = sub.add_parser('monitor', help='print events until interrupted')
    p.add_argument('--seconds', type=float, default=0, help='stop after this long (default: never)')

    sub.add_parser('bootloader', help='restart into ROM download mode')

    p = sub.add_parser('net', help='the network link: Wi-Fi and the InvenTree plugin (network builds only)')
    net = p.add_subparsers(dest='net_action')
    net.add_parser('status', help='what the link is doing (also the default)')
    q = net.add_parser('join', help='remember a Wi-Fi network and join it')
    q.add_argument('ssid')
    q.add_argument('--psk', default='', help='passphrase; none for an open network')
    q = net.add_parser('forget', help='forget a Wi-Fi network')
    q.add_argument('ssid')
    q = net.add_parser('server', help="the plugin's URL and the API token")
    q.add_argument('url', help='e.g. https://inventree.example/plugin/nfcscanner')
    q.add_argument('--token', help="an InvenTree API token of the scanner machine's user")
    q = net.add_parser('poll', help='pacing')
    q.add_argument('--poll-ms', type=int, help='idle interval, 100 to 60000')
    q.add_argument('--wait-s', type=int, help='long-poll hold, 0 to 300 (0: plain polling)')
    q = net.add_parser('enable', help='on the air')
    q = net.add_parser('disable', help='off the air')

    p = sub.add_parser('ota', help='fetch and install a firmware image, then restart (network builds only)')
    p.add_argument('url', help='where the .bin is served')
    p.add_argument('--sha256', help='its SHA-256, checked before it is used')
    p.add_argument('--file', help='compute --sha256 from this local copy of the image')

    p = sub.add_parser('ndef', help='print the NDEF message for a location, in hex')
    add_location(p)

    p = sub.add_parser('raw', help='send one line of JSON and print what comes back')
    p.add_argument('json')
    p.add_argument('--seconds', type=float, default=2.0)

    args = ap.parse_args()

    if args.command == 'ndef':
        print(build_ndef(args.host, args.pk, args.scheme).hex().upper())
        return 0

    dev = Device(args.port or find_port(), args.verbose)
    try:
        if args.command == 'info':
            show(dev.request({'cmd': 'info'}, on_event=show))
        elif args.command == 'program':
            cmd = {
                'cmd': 'program',
                'id': args.id,
                'ndef': build_ndef(args.host, args.pk, args.scheme).hex().upper(),
                'timeout_ms': int(args.timeout * 1000),
            }
            if args.overwrite:
                cmd['overwrite'] = True
            if args.pwd:
                cmd['pwd'] = args.pwd
            if args.pack:
                cmd['pack'] = args.pack
            if args.old_pwd:
                cmd['old_pwd'] = args.old_pwd
            return run_job(dev, cmd, args.timeout)
        elif args.command == 'wipe':
            cmd = {'cmd': 'wipe', 'id': args.id, 'timeout_ms': int(args.timeout * 1000)}
            if args.pwd:
                cmd['pwd'] = args.pwd
            return run_job(dev, cmd, args.timeout)
        elif args.command == 'cancel':
            rsp = dev.request({'cmd': 'cancel'}, on_event=show)
            show(rsp)
            for msg in dev.lines(0.5):
                show(msg)
            return 0 if rsp.get('ok') else 1
        elif args.command == 'hid':
            cmd = {'cmd': 'hid', 'enabled': args.state == 'on'}
            if args.persist:
                cmd['persist'] = True
            show(dev.request(cmd, on_event=show))
        elif args.command == 'bootloader':
            show(dev.request({'cmd': 'bootloader'}, on_event=show))
        elif args.command == 'net':
            cmd = {'cmd': 'net'}
            a = args.net_action or 'status'
            if a == 'join':
                cmd.update(action='join', ssid=args.ssid, psk=args.psk)
            elif a == 'forget':
                cmd.update(action='forget', ssid=args.ssid)
            elif a == 'server':
                cmd.update(action='server', url=args.url)
                if args.token:
                    cmd['token'] = args.token
            elif a == 'poll':
                cmd['action'] = 'poll'
                if args.poll_ms is not None:
                    cmd['poll_ms'] = args.poll_ms
                if args.wait_s is not None:
                    cmd['wait_s'] = args.wait_s
            elif a in ('enable', 'disable'):
                cmd['enabled'] = a == 'enable'
            rsp = dev.request(cmd, on_event=show)
            show(rsp)
            return 0 if rsp.get('ok') else 1
        elif args.command == 'ota':
            cmd = {'cmd': 'ota', 'url': args.url}
            if args.file:
                import hashlib
                with open(args.file, 'rb') as f:
                    cmd['sha256'] = hashlib.sha256(f.read()).hexdigest()
            elif args.sha256:
                cmd['sha256'] = args.sha256
            rsp = dev.request(cmd, on_event=show)
            show(rsp)
            if not rsp.get('ok'):
                return 1
            # Then the update's progress, until it restarts or gives up.
            for msg in dev.lines(300):
                show(msg)
                if msg.get('evt') == 'ota' and msg.get('state') in ('restarting', 'failed'):
                    return 0 if msg['state'] == 'restarting' else 1
            return 1
        elif args.command == 'raw':
            dev.send(json.loads(args.json))
            for msg in dev.lines(args.seconds):
                show(msg)
        elif args.command in ('log', 'monitor'):
            if args.command == 'log':
                show(dev.request({'cmd': 'log', 'level': args.level}, on_event=show))
                if args.level == 'off':
                    return 0
            seconds = getattr(args, 'seconds', 0) or 10 ** 9
            try:
                for msg in dev.lines(seconds):
                    show(msg)
            except KeyboardInterrupt:
                pass
    finally:
        dev.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
