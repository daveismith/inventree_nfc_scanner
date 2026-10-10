"""The C unit tests (host_test: NDEF, PN532 frames, NTAG operations, the protocol, the state
machine, the plugin exchange, the Wi-Fi policy), each reported as a test of its own.

host_test.elf runs them all with Unity, which prints `file:line:name:PASS` (or `:FAIL: why`,
`:IGNORE`) per test; the binary is run once, when the tests are collected.
"""

import os
import re
import subprocess
from pathlib import Path

import pytest

from conftest import ROOT

ELF = Path(os.environ.get("HOST_TEST", ROOT / "host_test" / "build" / "host_test.elf"))
LINE = re.compile(r"^(?P<where>[^:\s]+:\d+):(?P<name>\w+):(?P<status>PASS|FAIL|IGNORE)(?::\s*(?P<why>.*))?$")


def results():
    if not (ELF.is_file() and os.access(ELF, os.X_OK)):
        return None, f"{ELF} is not built (see tests/conftest.py)"
    run = subprocess.run([str(ELF)], capture_output=True, text=True, timeout=300)
    found = {}
    for line in run.stdout.splitlines():
        m = LINE.match(line.strip())
        if m:
            found[m["name"]] = (m["status"], m["why"] or "", m["where"])
    if not found:
        return None, f"{ELF} reported no tests (exit {run.returncode}):\n{run.stdout[-2000:]}"
    return found, ""


RESULTS, PROBLEM = results()


def test_the_unit_tests_ran():
    if RESULTS is None:
        if os.environ.get("REQUIRE_BUILDS"):
            pytest.fail(PROBLEM)
        pytest.skip(PROBLEM)
    assert len(RESULTS) > 0


@pytest.mark.parametrize("name", sorted(RESULTS or {}))
def test_unit(name):
    status, why, where = RESULTS[name]
    if status == "IGNORE":
        pytest.skip(f"{where}: ignored {why}")
    assert status == "PASS", f"{where}: {why}"
