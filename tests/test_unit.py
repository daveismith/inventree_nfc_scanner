"""The C unit tests (host_test: NDEF, PN532 frames, NTAG operations, the protocol, the state
machine, the plugin exchange, the Wi-Fi policy), each reported as a test of its own.

host_test.elf runs them all with Unity, which prints `file:line:name:PASS` (or `:FAIL: why`,
`:IGNORE`) per test; the binary is run once, when the tests are collected. A name two tests
share gets the line that runs each: `test_wipe@123`.
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
        return None, None, f"{ELF} is not built (see tests/conftest.py)"
    run = subprocess.run([str(ELF)], capture_output=True, text=True, timeout=300)
    lines = [m for m in (LINE.match(ln.strip()) for ln in run.stdout.splitlines()) if m]
    names = [m["name"] for m in lines]
    found = {}
    for m in lines:
        # Two groups may each have a test of the same name: those are told apart by the line
        # of test_main.c that runs them.
        key = m["name"] if names.count(m["name"]) == 1 else f"{m['name']}@{m['where'].rsplit(':', 1)[1]}"
        found[key] = (m["status"], m["why"] or "", m["where"])
    if not found:
        return None, None, f"{ELF} reported no tests (exit {run.returncode}):\n{run.stdout[-2000:]}"
    total = re.search(r"^(\d+) Tests \d+ Failures \d+ Ignored", run.stdout, re.M)
    return found, int(total[1]) if total else None, ""


RESULTS, UNITY_TOTAL, PROBLEM = results()


def test_the_unit_tests_ran():
    if RESULTS is None:
        if os.environ.get("REQUIRE_BUILDS"):
            pytest.fail(PROBLEM)
        pytest.skip(PROBLEM)
    # Each of Unity's tests is one here: none lost to a name two of them share.
    assert len(RESULTS) == UNITY_TOTAL, f"Unity ran {UNITY_TOTAL} tests; {len(RESULTS)} are reported here"


@pytest.mark.parametrize("name", sorted(RESULTS or {}))
def test_unit(name):
    status, why, where = RESULTS[name]
    if status == "IGNORE":
        pytest.skip(f"{where}: ignored {why}")
    assert status == "PASS", f"{where}: {why}"
