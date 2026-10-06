#!/usr/bin/env python3
"""The reader's network link, end to end, with no hardware and no InvenTree.

Starts tools/fake_plugin.py and the host simulator with its network link pointed at it, then
checks what the network plan asks of phase N1: a job queued on the plugin is written to the
simulated tag and its result arrives exactly once; with answers dropped at random nothing is
lost and nothing is done twice; a server that goes away and comes back gets what happened
meanwhile; a reader that restarts is noticed; what a remote link may not do is refused; and a
long poll delivers a job at once where plain polling takes up to a second.

    cd host_sim && idf.py --preview set-target linux && idf.py build
    python tools/test_sync.py [path/to/host_sim.elf]
"""

import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request

import serial

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from nfcprog import build_ndef  # noqa: E402

DEFAULT_ELF = os.path.join(HERE, '..', 'host_sim', 'build', 'host_sim.elf')
PORT = 8765
URL = f'http://127.0.0.1:{PORT}'
TOKEN = 'inv-test-token'
READER = 'nfc-sim000000'

checks = 0


def check(condition, what, detail=None):
    global checks
    checks += 1
    if not condition:
        print(f'FAIL: {what}' + (f' -- {detail}' if detail is not None else ''))
        sys.exit(1)
    print(f'ok   {what}')


def http(method, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(URL + path, data=data, method=method, headers={'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=5) as r:
        return json.loads(r.read() or b'null')


def start_plugin(**kw):
    args = [sys.executable, os.path.join(HERE, 'fake_plugin.py'), '--port', str(PORT), '--token', TOKEN, '--reader', READER]
    for k, v in kw.items():
        args += [f'--{k.replace("_", "-")}', str(v)]
    proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        try:
            http('GET', '/state')
            return proc
        except (urllib.error.URLError, ConnectionError):
            time.sleep(0.1)
    proc.kill()
    sys.exit('the fake plugin did not start')


def start_sim(elf, **env):
    e = dict(os.environ, SIM_SYNC_URL=URL, SIM_TOKEN=TOKEN, SIM_READER=READER, SIM_POLL_MS='300')
    e.update({k: str(v) for k, v in env.items()})
    proc = subprocess.Popen([elf], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, env=e)
    deadline = time.monotonic() + 10
    pty = None
    while time.monotonic() < deadline:
        line = proc.stdout.readline()
        if line.startswith('PTY '):
            pty = line.split()[1]
        if line.startswith('NET '):
            return proc, pty
        if not line and proc.poll() is not None:
            break
    proc.kill()
    sys.exit('the simulator did not start its network link')


class Pty:
    def __init__(self, path):
        self.port = serial.Serial(path, 115200, timeout=0.05)
        self.buf = b''

    def close(self):
        self.port.close()

    def send(self, line):
        self.port.write(line.encode() + b'\n')
        self.port.flush()

    def read(self, seconds=0.4):
        out = []
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.buf += self.port.read(4096)
            while b'\n' in self.buf:
                raw, self.buf = self.buf.split(b'\n', 1)
                if raw.strip():
                    out.append(json.loads(raw))
        return out


def wait_for(predicate, seconds, what):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        log = http('GET', '/log')
        if predicate(log):
            return log
        time.sleep(0.05)
    sys.exit(f'FAIL: gave up waiting for {what}; log: {json.dumps(http("GET", "/log"))[:800]}')


def of_job(log, job_id):
    return [m for m in log if m.get('id') == job_id]


def program_cmd(job_id, overwrite=False):
    cmd = {'cmd': 'program', 'id': job_id, 'ndef': build_ndef('inventree.example', 42).hex().upper(), 'timeout_ms': 60000}
    if overwrite:
        cmd['overwrite'] = True
    return cmd


def run_job(pty, job_id, overwrite=False, present_tag=True):
    """Queue a program job, present the tag, and wait for its result on the server."""
    http('POST', '/queue', program_cmd(job_id, overwrite))
    if present_tag:
        time.sleep(0.2)
        pty.send('!tap')
        pty.read(0.3)
    # Under a lossy link the reader backs off between tries, up to 30 s, so allow for that.
    return wait_for(lambda log: any(m.get('evt') in ('done', 'failed') for m in of_job(log, job_id)), 75, f'job {job_id}')


def main():
    elf = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_ELF
    plugin = start_plugin()
    sim, pty_path = start_sim(elf)
    pty = Pty(pty_path)
    try:
        pty.read(0.3)
        pty.send('!tag ntag215')
        pty.read(0.5)
        wait_for(lambda log: len(log) >= 1, 5, 'the first tap to reach the server')
        log = http('GET', '/log')
        check(log[0].get('evt') == 'tag' and log[0].get('uid') == '04A1B2C3D4E5F6', 'a tap outside a job reaches the server', log)

        # --- a job, start to finish, exactly once
        log = run_job(pty, 1)
        kinds = [(m.get('rsp') or m.get('evt')) for m in of_job(log, 1)]
        check(kinds == ['program', 'waiting', 'writing', 'done'], 'a queued job is written and reported once', kinds)
        check(of_job(log, 1)[0].get('ok') is True, 'and its command was accepted', of_job(log, 1)[0])
        state = http('GET', '/state')
        check(all(c['acked'] for c in state['commands']), 'the command is acknowledged', state['commands'])
        seqs = [m['seq'] for m in log]
        check(seqs == sorted(seqs) and len(set(seqs)) == len(seqs), 'messages arrive in order, each once', seqs)

        # --- answers dropped at random: nothing lost, nothing done twice
        http('POST', '/chaos', {'drop': 0.3})
        for job_id in range(2, 7):
            run_job(pty, job_id, overwrite=True)
        http('POST', '/chaos', {'drop': 0.0})
        log = http('GET', '/log')
        for job_id in range(2, 7):
            kinds = [(m.get('rsp') or m.get('evt')) for m in of_job(log, job_id)]
            check(kinds == ['program', 'waiting', 'writing', 'done'], f'job {job_id} ran once under a lossy link', kinds)
        state = http('GET', '/state')
        check(state['dropped'] > 0, 'answers really were dropped', state)
        seqs = [m['seq'] for m in log]
        check(len(set(seqs)) == len(seqs), 'no message was applied twice', seqs)

        # --- the server goes away and comes back (keeping its command numbering, as the real one does)
        last_seq = max(c['seq'] for c in http('GET', '/state')['commands'])
        plugin.kill()
        plugin.wait()
        time.sleep(0.5)
        pty.send('!tap')
        pty.read(0.3)
        pty.send('!tap')
        pty.read(0.3)
        rsp = [m for m in pty.read(0.2) if m.get('rsp') == 'net']
        pty.send('{"cmd":"net"}')
        rsp = [m for m in pty.read(0.5) if m.get('rsp') == 'net']
        time.sleep(2.5)
        pty.send('{"cmd":"net"}')
        rsp = [m for m in pty.read(0.5) if m.get('rsp') == 'net']
        check(rsp and rsp[0]['link'] == 'unreachable' and rsp[0]['queued'] >= 2, 'with the server gone, taps queue up and the link says so', rsp)
        plugin = start_plugin(seq_start=last_seq)
        log = wait_for(lambda log: sum(1 for m in log if m.get('evt') == 'tag') >= 2, 40, 'the queued taps after the server returns')
        check(all(m.get('evt') == 'tag' for m in log[:2]), 'and they are delivered when it is back', log)

        # --- what a remote link may not do
        http('POST', '/queue', {'cmd': 'bootloader'})
        log = wait_for(lambda log: any(m.get('rsp') == 'bootloader' for m in log), 5, 'the bootloader refusal')
        rsp = next(m for m in log if m.get('rsp') == 'bootloader')
        check(rsp.get('ok') is False and rsp.get('error') == 'not_allowed', 'bootloader from the plugin is refused', rsp)
        http('POST', '/queue', {'cmd': 'net', 'action': 'join', 'ssid': 'x'})
        log = wait_for(lambda log: any(m.get('rsp') == 'net' for m in log), 5, 'the net refusal')
        rsp = next(m for m in log if m.get('rsp') == 'net')
        check(rsp.get('error') == 'not_allowed', 'so is changing the network settings', rsp)

        # --- the reader restarts: a new boot, and the plugin notices
        boots_before = http('GET', '/state')['boots']
        pty.close()
        sim.kill()
        sim.wait()
        sim, pty_path = start_sim(elf)
        pty = Pty(pty_path)
        pty.read(0.3)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and len(http('GET', '/state')['boots']) == len(boots_before):
            time.sleep(0.1)
        boots = http('GET', '/state')['boots']
        check(len(boots) == len(boots_before) + 1, 'a restarted reader calls with a new boot number', boots)

        # --- the plugin's command for the reader survives the restart: a job queued while it
        #     was down is run when it is back
        pty.send('!tag ntag215')
        pty.read(0.3)
        log = run_job(pty, 20, overwrite=True)
        kinds = [(m.get('rsp') or m.get('evt')) for m in of_job(log, 20)]
        check(kinds == ['program', 'waiting', 'writing', 'done'], 'and runs a job as before', kinds)

        # --- pacing: plain polling delivers within the interval, a long poll at once
        t0 = time.monotonic()
        http('POST', '/queue', {'cmd': 'info'})
        wait_for(lambda log: any(m.get('rsp') == 'info' for m in log), 5, 'info over plain polling')
        plain = time.monotonic() - t0
        check(plain < 0.8, f'plain polling at 300 ms delivered a command in {plain:.2f}s', plain)

        pty.close()
        sim.kill()
        sim.wait()
        http('POST', '/reset')
        sim, pty_path = start_sim(elf, SIM_WAIT_S='5', SIM_POLL_MS='5000')
        pty = Pty(pty_path)
        pty.read(0.3)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and http('GET', '/state')['calls'] == 0:
            time.sleep(0.05)
        time.sleep(0.3)                                 # the hold is in progress now
        t0 = time.monotonic()
        http('POST', '/queue', {'cmd': 'info'})
        wait_for(lambda log: any(m.get('rsp') == 'info' for m in log), 8, 'info over a long poll')
        held = time.monotonic() - t0
        check(held < 1.0, f'a long poll (5 s hold, 5 s idle) delivered a command in {held:.2f}s', held)
        calls = http('GET', '/state')['calls']
        time.sleep(3)
        check(http('GET', '/state')['calls'] - calls <= 2, 'and the reader is not polling in a tight loop meanwhile')

        print(f'\n{checks} checks passed')
    finally:
        pty.close()
        sim.kill()
        plugin.kill()


if __name__ == '__main__':
    main()
