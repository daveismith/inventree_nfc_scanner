#!/usr/bin/env python3
"""End-to-end check of the harness against the host simulator.

Starts host_sim (the firmware's state machine, protocol and tag logic over a simulated NTAG
on a pty) and drives it the way a user would: through nfcprog.py, with the simulated tag
moved by the simulator's '!' control lines.

    cd host_sim && idf.py --preview set-target linux && idf.py build
    python tools/test_sim.py [path/to/host_sim.elf]

Exits non-zero on the first failed check.
"""

import json
import os
import subprocess
import sys
import time

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
NFCPROG = os.path.join(HERE, 'nfcprog.py')
DEFAULT_ELF = os.path.join(HERE, '..', 'host_sim', 'build', 'host_sim.elf')

HOST = 'inventree.example'
URI = f'https://{HOST}/web/stock/location/42'
UID = '04A1B2C3D4E5F6'

checks = 0


def check(condition, what):
    global checks
    checks += 1
    if not condition:
        print(f'FAIL: {what}')
        sys.exit(1)
    print(f'ok   {what}')


def start_sim(elf):
    proc = subprocess.Popen([elf], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        line = proc.stdout.readline()
        if line.startswith('PTY '):
            return proc, line.split()[1]
        if not line and proc.poll() is not None:
            break
    proc.kill()
    sys.exit('the simulator did not report its pty')


class Link:
    """A direct connection, for the simulator's control lines and for raw protocol lines."""

    def __init__(self, path):
        self.port = serial.Serial(path, 115200, timeout=0.05)
        self.buf = b''

    def close(self):
        self.port.close()

    def send(self, line):
        self.port.write(line.encode() + b'\n')
        self.port.flush()

    def read(self, seconds=0.4, until=None):
        """Objects received within `seconds`, stopping early once `until(obj)` is true."""
        out = []
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.buf += self.port.read(4096)
            while b'\n' in self.buf:
                raw, self.buf = self.buf.split(b'\n', 1)
                if raw.strip():
                    out.append(json.loads(raw))
                    if until and until(out[-1]):
                        return out
        return out


def sim(path, *lines, seconds=0.4):
    """Send control or protocol lines; return everything that came back."""
    link = Link(path)
    try:
        for line in lines:
            link.send(line)
        return link.read(seconds)
    finally:
        link.close()


def nfcprog(path, *args):
    """Run nfcprog.py; return (exit code, the objects it printed)."""
    result = subprocess.run([sys.executable, NFCPROG, '--port', path, *args],
                            capture_output=True, text=True, timeout=60)
    objs = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
    return result.returncode, objs


def events(objs, name):
    return [o for o in objs if o.get('evt') == name]


NODE_NDEF = """
const fs = require('fs');
const src = fs.readFileSync(process.argv[1], 'utf8').match(/<script>([\\s\\S]*)<\\/script>/)[1];
const m = { exports: {} };
new Function('module', src)(m);
const [host, pk, scheme] = process.argv.slice(2);
console.log(m.exports.toHex(m.exports.buildNdef(host, Number(pk), scheme)));
"""


def check_builders_agree():
    """The page and nfcprog.py must put the same bytes on a tag."""
    import shutil

    sys.path.insert(0, HERE)
    from nfcprog import build_ndef

    node = shutil.which('node')
    if not node:
        print('skip the WebSerial page\'s NDEF builder: node is not installed')
        return
    page = os.path.join(HERE, 'webserial.html')
    for host, pk, scheme in [(HOST, 42, 'https'), ('10.0.0.5:8000', 7, 'http'), ('a.b', 123456, 'https')]:
        js = subprocess.run([node, '-e', NODE_NDEF, page, host, str(pk), scheme],
                            capture_output=True, text=True, timeout=30).stdout.strip()
        check(js == build_ndef(host, pk, scheme).hex().upper(), f'page and nfcprog.py build the same NDEF for {scheme}://{host} pk {pk}')


def main():
    elf = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else DEFAULT_ELF)
    check_builders_agree()
    proc, pty = start_sim(elf)
    try:
        run(pty)
    finally:
        proc.kill()
    print(f'\n{checks} checks passed')


def run(pty):
    # --- info
    code, out = nfcprog(pty, 'info')
    check(code == 0 and out[-1].get('rsp') == 'info' and out[-1]['ok'], 'info answers')
    check(out[-1]['proto'] == 1 and out[-1]['state'] == 'idle' and out[-1]['job'] is None, 'info reports idle')

    # --- a blank tag is looked up, and types nothing
    out = sim(pty, '!tag ntag215')
    check({'sim': 'ok'} in out, 'control line acknowledged')
    tag = events(out, 'tag')
    check(len(tag) == 1 and tag[0]['uid'] == UID and tag[0]['type'] == 'ntag215' and 'text' not in tag[0],
          'blank tag reported without text')
    check(not [o for o in out if o.get('sim') == 'hid'], 'nothing typed for a blank tag')

    # --- program it: the tag is already on the reader, so the job runs at once
    code, out = nfcprog(pty, 'program', '--host', HOST, '--pk', '42', '--id', '7')
    check(code == 0, 'program exits 0')
    check(out[0] == {'rsp': 'program', 'ok': True, 'id': 7}, 'program acknowledged')
    check([o.get('evt') for o in out[1:]] == ['waiting', 'writing', 'done'], 'waiting, writing, done in order')
    check(out[-1] == {'evt': 'done', 'id': 7, 'uid': UID, 'type': 'ntag215', 'protected': False},
          'done carries the UID for the barcode link')

    # --- the firmware's parser reads back what the Python builder wrote
    out = sim(pty, '!tap')
    tag = events(out, 'tag')
    check(len(tag) == 1 and tag[0].get('text') == 'INV-SL42' and tag[0].get('uri') == URI,
          'tap reports the text and URI records nfcprog.py built')
    check({'sim': 'hid', 'text': 'INV-SL42'} in out, 'tap types the text record')
    check(len(events(out, 'tag_removed')) == 1, 'removal reported')

    # --- no overwrite unless asked
    code, out = nfcprog(pty, 'program', '--host', HOST, '--pk', '43')
    failed = events(out, 'failed')
    check(code == 1 and failed and failed[0]['error'] == 'not_blank', 'second program refused: not_blank')
    check(failed[0].get('text') == 'INV-SL42' and failed[0].get('uri') == URI, 'not_blank says what is there')
    check(not events(out, 'writing'), 'nothing written')

    # --- overwrite, and protect
    code, out = nfcprog(pty, 'program', '--host', HOST, '--pk', '43', '--overwrite', '--pwd', 'A1B2C3D4', '--pack', '1234')
    check(code == 0 and out[-1].get('protected') is True, 'overwrite with password: done, protected')
    out = sim(pty, '!tap')
    tag = events(out, 'tag')
    check(tag and tag[0].get('text') == 'INV-SL43' and tag[0].get('protected') is True,
          'protected tag still reads, and says it is protected')

    code, out = nfcprog(pty, 'program', '--host', HOST, '--pk', '44', '--overwrite')
    check(code == 1 and events(out, 'failed')[0]['error'] == 'auth_required', 'no password: auth_required')
    code, out = nfcprog(pty, 'program', '--host', HOST, '--pk', '44', '--overwrite', '--pwd', '00000000')
    check(code == 1 and events(out, 'failed')[0]['error'] == 'auth_failed', 'wrong password: auth_failed')
    code, out = nfcprog(pty, 'program', '--host', HOST, '--pk', '44', '--overwrite', '--pwd', 'A1B2C3D4')
    check(code == 0, 'right password: done')

    # --- wipe
    code, out = nfcprog(pty, 'wipe')
    check(code == 1 and events(out, 'failed')[0]['error'] == 'auth_required', 'wipe without password refused')
    code, out = nfcprog(pty, 'wipe', '--pwd', 'A1B2C3D4')
    check(code == 0 and out[-1].get('protected') is False, 'wipe with password: done, unprotected')
    out = sim(pty, '!tap')
    check('text' not in events(out, 'tag')[0], 'wiped tag is blank')

    # --- pulled away mid-write
    sim(pty, '!tag ntag215', '!tear 3')
    code, out = nfcprog(pty, 'program', '--host', HOST, '--pk', '42')
    check(code == 1 and [o.get('evt') for o in out[1:4]] == ['waiting', 'writing', 'failed']
          and events(out, 'failed')[0]['error'] == 'tag_removed', 'torn write: writing, then failed tag_removed')
    out = sim(pty, '!tap')
    tag = events(out, 'tag')
    check(tag and 'text' not in tag[0] and 'error' not in tag[0], 'torn tag reads as empty, not broken')
    code, out = nfcprog(pty, 'program', '--host', HOST, '--pk', '42')
    check(code == 0, 'and takes the same job when presented again')

    # --- other tags
    sim(pty, '!remove', '!tag classic')
    code, out = nfcprog(pty, 'program', '--host', HOST, '--pk', '42')
    check(code == 1 and events(out, 'failed')[0]['error'] == 'wrong_tag_type', 'wrong tag type')
    sim(pty, '!remove', '!tag ntag213')
    code, out = nfcprog(pty, 'program', '--host', 'a-very-long-host-name-' + 'x' * 100 + '.example', '--pk', '42')
    check(code == 1 and events(out, 'failed')[0]['error'] == 'too_large', 'too large for an NTAG213')

    # --- timeout, with no tag
    sim(pty, '!remove')
    started = time.monotonic()
    code, out = nfcprog(pty, 'program', '--host', HOST, '--pk', '42', '--timeout', '1')
    took = time.monotonic() - started
    check(code == 1 and events(out, 'failed')[0]['error'] == 'timeout', 'no tag: timeout')
    check(1.0 <= took < 4.0, f'after about a second ({took:.1f}s)')

    # --- busy, cancel, two tags
    link = Link(pty)
    link.send(json.dumps({'cmd': 'program', 'id': 1, 'ndef': 'D101035400656E'}))
    out = link.read(until=lambda o: o.get('evt') == 'waiting')
    link.send(json.dumps({'cmd': 'program', 'id': 2, 'ndef': 'D101035400656E'}))
    out = link.read(until=lambda o: o.get('rsp') == 'program')
    check(out[-1] == {'rsp': 'program', 'ok': False, 'id': 2, 'error': 'busy'}, 'second job: busy')
    link.send('{"cmd":"cancel"}')
    out = link.read()
    check({'rsp': 'cancel', 'ok': True} in out and {'evt': 'failed', 'id': 1, 'error': 'cancelled'} in out,
          'cancel ends the job')
    link.send('{"cmd":"cancel"}')
    out = link.read()
    check(out == [{'rsp': 'cancel', 'ok': False, 'error': 'no_job'}], 'cancel with no job: no_job')

    link.send(json.dumps({'cmd': 'program', 'id': 3, 'ndef': 'D101035400656E'}))
    link.read(until=lambda o: o.get('evt') == 'waiting')
    link.send('!two')
    out = link.read()
    check({'evt': 'failed', 'id': 3, 'error': 'multiple_tags'} in out, 'two tags: multiple_tags')

    # --- malformed input changes nothing
    link.send('this is not json')
    out = link.read()
    check(out and out[0].get('evt') == 'error' and out[0]['error'] == 'bad_json', 'garbage: bad_json')
    link.send('{"cmd":"program","id":4,"ndef":"D1010B"}')
    out = link.read()
    check(out and out[0].get('rsp') == 'program' and out[0]['error'] == 'bad_arg' and out[0]['id'] == 4,
          'truncated NDEF: bad_arg, with the id')
    link.send('x' * 3000)
    out = link.read()
    check(out == [{'evt': 'error', 'error': 'line_too_long'}], 'over-long line: line_too_long')
    link.send('{"cmd":"info"}')
    out = link.read()
    check(out and out[0].get('state') == 'idle', 'still idle afterwards')

    # --- reader failure
    link.send('!nfc off')
    link.send(json.dumps({'cmd': 'program', 'id': 5, 'ndef': 'D101035400656E'}))
    out = link.read()
    check({'rsp': 'program', 'ok': False, 'id': 5, 'error': 'nfc_error'} in out, 'reader down: nfc_error')
    link.send('!nfc on')
    link.read()
    link.close()

    # --- hid off for a session
    code, out = nfcprog(pty, 'hid', 'off')
    check(code == 0 and out[-1] == {'rsp': 'hid', 'ok': True, 'enabled': False}, 'hid off acknowledged')
    out = sim(pty, '!tag ntag215')
    code, _ = nfcprog(pty, 'program', '--host', HOST, '--pk', '42')
    out = sim(pty, '!tap')
    check(events(out, 'tag')[0].get('text') == 'INV-SL42' and not [o for o in out if o.get('sim') == 'hid'],
          'with hid off, the tap is reported and not typed')

    # --- a firmware over USB
    import base64
    import hashlib
    import tempfile

    image = b'fw=0.3.0-sim\n' + os.urandom(5000)
    with tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as f:
        f.write(image)
    try:
        code, out = nfcprog(pty, 'update', f.name)
    finally:
        os.unlink(f.name)
    ota = [o.get('state') for o in events(out, 'ota')]
    check(code == 0 and ota == ['downloading', 'restarting'], f'an image sent over USB is taken and restarted into ({ota})')
    code, out = nfcprog(pty, 'info')
    check(out[-1].get('fw') == '0.3.0-sim', 'and the simulator then runs it')

    link = Link(pty)
    digest = hashlib.sha256(image).hexdigest()
    link.send(json.dumps({'cmd': 'ota_begin', 'id': 9, 'size': len(image), 'sha256': '00' * 32}))
    link.read(0.2)
    for at in range(0, len(image), 768):
        link.send(json.dumps({'cmd': 'ota_data', 'id': 9, 'at': at, 'data': base64.b64encode(image[at:at + 768]).decode()}))
        link.read(0.05)
    link.send('{"cmd":"ota_end","id":9}')
    out = link.read(0.5)
    check({'rsp': 'ota_end', 'ok': False, 'id': 9, 'error': 'verify_failed', 'detail': 'sha256 does not match'} in out
          and {'evt': 'ota', 'state': 'failed', 'detail': 'sha256 does not match'} in out,
          'an image that is not the one named is refused, and nothing restarts')

    link.send(json.dumps({'cmd': 'ota_begin', 'id': 10, 'size': len(image), 'sha256': digest}))
    link.read(0.2)
    link.send(json.dumps({'cmd': 'program', 'id': 11, 'ndef': 'D101035400656E'}))
    out = link.read(0.3)
    check({'rsp': 'program', 'ok': False, 'id': 11, 'error': 'busy', 'detail': 'a firmware update is in progress'} in out,
          'no job begins while an image is arriving')
    link.send(json.dumps({'cmd': 'ota_data', 'id': 10, 'at': 768, 'data': base64.b64encode(image[:768]).decode()}))
    out = link.read(0.3)
    check({'rsp': 'ota_data', 'ok': False, 'id': 10, 'error': 'bad_arg', 'detail': 'at: expected 0'} in out,
          'a piece out of order ends the update')
    link.send('{"cmd":"ota_end","id":10}')
    out = link.read(0.3)
    check(out and out[0].get('error') == 'no_job', 'and nothing is left of it')
    link.close()
    code, out = nfcprog(pty, 'info')
    check(out[-1].get('fw') == '0.3.0-sim' and out[-1].get('state') == 'idle', 'the firmware is unchanged by the failures')

    # --- bootloader
    code, out = nfcprog(pty, 'bootloader')
    check(code == 0 and out[-1] == {'rsp': 'bootloader', 'ok': True}, 'bootloader acknowledged')


if __name__ == '__main__':
    main()
