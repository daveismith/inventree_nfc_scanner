"""Fixtures for the firmware's host-side tests: the host simulator, the fake plugin, nfcprog.py.

Nothing here needs the board. The programs under test are built first:

    (cd host_test && idf.py --preview set-target linux && idf.py build)
    (cd host_sim && idf.py --preview set-target linux && idf.py build)

HOST_TEST and HOST_SIM name other builds of them. Without one, its tests are skipped, or with
REQUIRE_BUILDS set (as CI sets it), fail.
"""

from __future__ import annotations

import json
import os
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

import jsonschema
import pytest
import serial

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))

UID = "04A1B2C3D4E5F6"  # the simulated tag's

# docs/protocol.schema.json: everything the device sends is held to it.
_SCHEMA = json.loads((ROOT / "docs" / "protocol.schema.json").read_text())
_MESSAGE = jsonschema.Draft202012Validator(
    {"$schema": _SCHEMA["$schema"], "$defs": _SCHEMA["$defs"], "$ref": "#/$defs/message"}
)


def check_message(obj):
    """Fail the test if `obj`, a line from the device, is not a message as the schema has it."""
    errors = sorted(_MESSAGE.iter_errors(obj), key=lambda e: list(e.path))
    if errors:
        pytest.fail(f"not a protocol message: {json.dumps(obj)}: {errors[0].message}")


def built(env: str, default: Path) -> Path:
    path = Path(os.environ.get(env, default))
    if not (path.is_file() and os.access(path, os.X_OK)):
        message = f"{path} is not built (see tests/conftest.py)"
        if os.environ.get("REQUIRE_BUILDS"):
            pytest.fail(message)
        pytest.skip(message)
    return path


@pytest.fixture(scope="session")
def sim_elf():
    return built("HOST_SIM", ROOT / "host_sim" / "build" / "host_sim.elf")


# The simulator --------------------------------------------------------------------------------


class Link:
    """A direct connection to the simulator's pty: protocol lines, and its '!' control lines."""

    def __init__(self, path):
        self.port = serial.Serial(path, 115200, timeout=0.05)
        self.buf = b""

    def close(self):
        self.port.close()

    def send(self, line):
        self.port.write(line.encode() + b"\n")
        self.port.flush()

    def read(self, seconds=0.4, until=None):
        """Objects received within `seconds`, stopping early once `until(obj)` is true."""
        out = []
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.buf += self.port.read(4096)
            while b"\n" in self.buf:
                raw, self.buf = self.buf.split(b"\n", 1)
                if raw.strip():
                    out.append(json.loads(raw))
                    if "sim" not in out[-1]:        # the simulator's own answers to '!' lines
                        check_message(out[-1])
                    if until and until(out[-1]):
                        return out
        return out


class Sim:
    """A running host simulator."""

    def __init__(self, elf, env=None, wait_for_net=False):
        e = dict(os.environ, **(env or {}))
        self.proc = subprocess.Popen(
            [str(elf)], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, env=e
        )
        self.pty = None
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            line = self.proc.stdout.readline()
            if line.startswith("PTY "):
                self.pty = line.split()[1]
                if not wait_for_net:
                    return
            if line.startswith("NET ") and self.pty:
                return
            if not line and self.proc.poll() is not None:
                break
        self.proc.kill()
        raise RuntimeError("the simulator did not start" + (" its network link" if wait_for_net else ""))

    def link(self) -> Link:
        return Link(self.pty)

    def send(self, *lines, seconds=0.4):
        """Send control or protocol lines on a fresh connection; return what came back."""
        link = self.link()
        try:
            for line in lines:
                link.send(line)
            return link.read(seconds)
        finally:
            link.close()

    def nfcprog(self, *args):
        """Run tools/nfcprog.py against it: (exit code, the objects it printed)."""
        result = subprocess.run(
            [sys.executable, str(TOOLS / "nfcprog.py"), "--port", self.pty, *args],
            capture_output=True,
            text=True,
            timeout=60,
        )
        objs = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
        return result.returncode, objs

    def stop(self):
        self.proc.kill()
        self.proc.wait(timeout=5)


@pytest.fixture
def sim(sim_elf):
    """A fresh simulator for the test: a blank NTAG215 can be put on its reader with
    `sim.send("!tag ntag215")`."""
    s = Sim(sim_elf)
    yield s
    s.stop()


def events(objs, name):
    return [o for o in objs if o.get("evt") == name]


# The fake plugin (tools/fake_plugin.py), for the network link --------------------------------


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class FakePlugin:
    TOKEN = "inv-test-token"
    READER = "nfc-sim000000"

    def __init__(self):
        self.port = free_port()
        self.url = f"http://127.0.0.1:{self.port}"
        self.proc = None

    def start(self, **options):
        args = [sys.executable, str(TOOLS / "fake_plugin.py"), "--port", str(self.port),
                "--token", self.TOKEN, "--reader", self.READER]
        for key, value in options.items():
            args += [f"--{key.replace('_', '-')}", str(value)]
        self.proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            try:
                self.http("GET", "/state")
                return self
            except (urllib.error.URLError, ConnectionError):
                time.sleep(0.1)
        self.stop()
        raise RuntimeError("the fake plugin did not start")

    def stop(self):
        if self.proc:
            self.proc.kill()
            self.proc.wait(timeout=5)
            self.proc = None

    def http(self, method, path, body=None):
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(
            self.url + path, data=data, method=method,
            headers={"Content-Type": "application/json", "Authorization": f"Token {self.TOKEN}"},
        )
        with urllib.request.urlopen(req, timeout=5) as r:
            return json.loads(r.read() or b"null")

    def log(self):
        log = self.http("GET", "/log")
        for m in log:
            check_message({k: v for k, v in m.items() if k != "boot"})  # `boot` is the fake's
        return log

    def state(self):
        return self.http("GET", "/state")

    def queue(self, cmd):
        return self.http("POST", "/queue", cmd)

    def wait_for(self, predicate, seconds, what):
        """The log, once `predicate(log)` holds."""
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            log = self.log()
            if predicate(log):
                return log
            time.sleep(0.05)
        pytest.fail(f"gave up waiting for {what}; log: {json.dumps(self.log())[:800]}")


@pytest.fixture
def plugin():
    p = FakePlugin().start()
    yield p
    p.stop()


@pytest.fixture
def net_sim(sim_elf, plugin):
    """`net_sim(**env)`: a simulator whose network link calls the fake plugin (300 ms polling,
    firmware 0.2.0-sim, unless env says otherwise). Started ones are stopped afterwards."""
    started = []

    def start(**env):
        e = {
            "SIM_SYNC_URL": plugin.url,
            "SIM_TOKEN": plugin.TOKEN,
            "SIM_READER": plugin.READER,
            "SIM_FW": "0.2.0-sim",
            "SIM_POLL_MS": "300",
        }
        e.update({k: str(v) for k, v in env.items()})
        s = Sim(sim_elf, env=e, wait_for_net=True)
        started.append(s)
        return s

    yield start
    for s in started:
        s.stop()
