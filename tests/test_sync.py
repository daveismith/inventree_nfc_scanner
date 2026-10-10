"""The reader's network link end to end, with no hardware and no InvenTree: the host simulator
with its network link pointed at tools/fake_plugin.py (docs/network-transport-plan.md, phase N1).

A job queued on the plugin is written to the simulated tag and its result arrives exactly once;
with answers dropped at random nothing is lost or done twice; a server that goes away and comes
back gets what happened meanwhile; a reader that restarts is noticed; what a remote link may not
do is refused; and a long poll delivers a job at once where plain polling takes up to a second.

Each test has a fake plugin and a simulator of its own (`plugin`, `net_sim`).
"""

import time

import pytest

from conftest import UID
from nfcprog import build_ndef


def of_job(log, job_id):
    return [m for m in log if m.get("id") == job_id]


def kinds(log, job_id):
    return [(m.get("rsp") or m.get("evt")) for m in of_job(log, job_id)]


def program_cmd(job_id, overwrite=False):
    cmd = {"cmd": "program", "id": job_id, "ndef": build_ndef("inventree.example", 42).hex().upper(),
           "timeout_ms": 60000}
    if overwrite:
        cmd["overwrite"] = True
    return cmd


def run_job(plugin, link, job_id, overwrite=False):
    """Queue a program job, present the tag, and wait for its result on the server."""
    plugin.queue(program_cmd(job_id, overwrite))
    time.sleep(0.2)
    link.send("!tap")
    link.read(0.3)
    # Under a lossy link the reader backs off between tries, up to 30 s, so allow for that.
    return plugin.wait_for(lambda log: any(m.get("evt") in ("done", "failed") for m in of_job(log, job_id)),
                           75, f"job {job_id}")


@pytest.fixture
def reader(net_sim):
    """A networked simulator with a blank tag on its reader, and a link to its pty."""
    sim = net_sim()
    link = sim.link()
    link.read(0.3)
    link.send("!tag ntag215")
    link.read(0.5)
    yield sim, link
    link.close()


def test_a_tap_reaches_the_server_with_the_firmware_version(plugin, reader):
    log = plugin.wait_for(lambda log: len(log) >= 1, 5, "the first tap")
    assert log[0].get("evt") == "tag" and log[0].get("uid") == UID
    assert plugin.state()["fw"] == "0.2.0-sim", "every call says which firmware the reader runs"


def test_a_queued_job_is_written_and_reported_once(plugin, reader):
    _, link = reader
    log = run_job(plugin, link, 1)
    assert kinds(log, 1) == ["program", "waiting", "writing", "done"]
    assert of_job(log, 1)[0].get("ok") is True
    assert all(c["acked"] for c in plugin.state()["commands"])
    seqs = [m["seq"] for m in log]
    assert seqs == sorted(seqs) and len(set(seqs)) == len(seqs), "in order, each once"


def test_on_a_lossy_link_nothing_is_lost_or_done_twice(plugin, reader):
    _, link = reader
    plugin.http("POST", "/chaos", {"drop": 0.3})
    for job_id in range(2, 7):
        run_job(plugin, link, job_id, overwrite=True)
    plugin.http("POST", "/chaos", {"drop": 0.0})
    log = plugin.log()
    for job_id in range(2, 7):
        assert kinds(log, job_id) == ["program", "waiting", "writing", "done"], f"job {job_id}"
    assert plugin.state()["dropped"] > 0, "answers really were dropped"
    seqs = [m["seq"] for m in log]
    assert len(set(seqs)) == len(seqs), "no message was applied twice"


def test_a_backlog_beyond_the_answer_buffer_drains_in_order(plugin, reader):
    """Answers carry two commands each, as the plugin caps them, so forty queued commands
    (far more than the reader's 8 KB answer buffer) still drain, each once, in order."""
    before = len(plugin.log())
    for i in range(40):
        plugin.queue({"cmd": "info", "id": 1000 + i})
    log = plugin.wait_for(lambda log: sum(1 for m in log[before:] if m.get("rsp") == "info") >= 40, 60,
                          "forty queued commands")
    assert [m.get("id") for m in log[before:] if m.get("rsp") == "info"] == list(range(1000, 1040))


def test_a_server_that_goes_away_gets_what_happened_when_it_is_back(plugin, reader):
    _, link = reader
    run_job(plugin, link, 1)  # so the server has numbered commands to carry on from
    last_seq = max(c["seq"] for c in plugin.state()["commands"])
    plugin.stop()
    time.sleep(0.5)
    for _ in range(2):
        link.send("!tap")
        link.read(0.3)
    time.sleep(2.5)
    link.send('{"cmd":"net"}')
    rsp = [m for m in link.read(0.5) if m.get("rsp") == "net"]
    assert rsp and rsp[0]["link"] == "unreachable" and rsp[0]["queued"] >= 2, rsp

    plugin.start(seq_start=last_seq)  # keeping its numbering, as the real one does
    log = plugin.wait_for(lambda log: sum(1 for m in log if m.get("evt") == "tag") >= 2, 40,
                          "the queued taps")
    # Anything else still unacknowledged when it went comes along too, first, being older.
    taps = [m for m in log if m.get("evt") == "tag"]
    assert all(m.get("evt") != "tag" for m in log[: log.index(taps[0])])


@pytest.mark.parametrize(
    "cmd, rsp",
    [({"cmd": "bootloader"}, "bootloader"), ({"cmd": "net", "action": "join", "ssid": "x"}, "net")],
    ids=["bootloader", "network settings"],
)
def test_a_remote_link_may_not(plugin, reader, cmd, rsp):
    plugin.queue(cmd)
    log = plugin.wait_for(lambda log: any(m.get("rsp") == rsp for m in log), 5, f"the {rsp} refusal")
    answer = next(m for m in log if m.get("rsp") == rsp)
    assert answer.get("ok") is False and answer.get("error") == "not_allowed"


def test_a_restarted_reader_calls_with_a_new_boot_and_runs_jobs_as_before(plugin, net_sim):
    first = net_sim()
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline and not plugin.state()["boots"]:
        time.sleep(0.1)
    boots_before = plugin.state()["boots"]
    first.stop()
    second = net_sim()
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline and len(plugin.state()["boots"]) == len(boots_before):
        time.sleep(0.1)
    assert len(plugin.state()["boots"]) == len(boots_before) + 1
    link = second.link()
    try:
        link.send("!tag ntag215")
        link.read(0.3)
        log = run_job(plugin, link, 20, overwrite=True)
    finally:
        link.close()
    assert kinds(log, 20) == ["program", "waiting", "writing", "done"]


def test_plain_polling_delivers_within_the_interval(plugin, reader):
    t0 = time.monotonic()
    plugin.queue({"cmd": "info"})
    plugin.wait_for(lambda log: any(m.get("rsp") == "info" for m in log), 5, "info over plain polling")
    took = time.monotonic() - t0
    assert took < 0.8, f"polling every 300 ms delivered a command in {took:.2f}s"


def test_a_long_poll_delivers_at_once_without_polling_in_a_tight_loop(plugin, net_sim):
    net_sim(SIM_WAIT_S=5, SIM_POLL_MS=5000)
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline and plugin.state()["calls"] == 0:
        time.sleep(0.05)
    time.sleep(0.3)  # the hold is in progress now
    t0 = time.monotonic()
    plugin.queue({"cmd": "info"})
    plugin.wait_for(lambda log: any(m.get("rsp") == "info" for m in log), 8, "info over a long poll")
    held = time.monotonic() - t0
    assert held < 1.0, f"a long poll (5 s hold, 5 s idle) delivered a command in {held:.2f}s"
    calls = plugin.state()["calls"]
    time.sleep(3)
    assert plugin.state()["calls"] - calls <= 2, "the reader is not polling in a tight loop meanwhile"
