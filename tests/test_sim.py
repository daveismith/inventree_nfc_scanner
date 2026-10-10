"""The firmware's logic end to end, through the host simulator: driven as a user would, with
tools/nfcprog.py, and with the simulated tag moved by the simulator's '!' control lines.

Each test has a simulator of its own (the `sim` fixture), with no tag on the reader.
"""

import base64
import hashlib
import json
import os
import shutil
import subprocess
import time

import pytest

from conftest import TOOLS, UID, events

HOST = "inventree.example"
URI = f"https://{HOST}/web/stock/location/42"
NDEF_EMPTY_TEXT = "D101035400656E"


def program(sim, pk, *args):
    return sim.nfcprog("program", "--host", HOST, "--pk", str(pk), *args)


def programmed(sim, pk=42, *args):
    """A tag on the reader, programmed for location `pk`."""
    sim.send("!tag ntag215")
    code, out = program(sim, pk, *args)
    assert code == 0, out
    return out


def protected(sim):
    """A tag on the reader programmed for 43 and protected with A1B2C3D4."""
    programmed(sim, 42)
    code, out = program(sim, 43, "--overwrite", "--pwd", "A1B2C3D4", "--pack", "1234")
    assert code == 0 and out[-1].get("protected") is True, out


def failure(out):
    return events(out, "failed")[0]["error"]


# Looking up ----------------------------------------------------------------------------------


def test_info_reports_idle(sim):
    code, out = sim.nfcprog("info")
    assert code == 0 and out[-1].get("rsp") == "info" and out[-1]["ok"]
    assert out[-1]["proto"] == 1 and out[-1]["state"] == "idle" and out[-1]["job"] is None


def test_a_blank_tag_is_reported_without_text_and_types_nothing(sim):
    out = sim.send("!tag ntag215")
    assert {"sim": "ok"} in out
    (tag,) = events(out, "tag")
    assert tag["uid"] == UID and tag["type"] == "ntag215" and "text" not in tag
    assert not [o for o in out if o.get("sim") == "hid"]


# Programming -------------------------------------------------------------------------------


def test_a_tag_on_the_reader_is_programmed_at_once(sim):
    sim.send("!tag ntag215")
    code, out = program(sim, 42, "--id", "7")
    assert code == 0
    assert out[0] == {"rsp": "program", "ok": True, "id": 7}
    assert [o.get("evt") for o in out[1:]] == ["waiting", "writing", "done"]
    assert out[-1] == {"evt": "done", "id": 7, "uid": UID, "type": "ntag215", "protected": False}


def test_the_firmware_reads_back_what_nfcprog_wrote(sim):
    programmed(sim, 42)
    out = sim.send("!tap")
    (tag,) = events(out, "tag")
    assert tag.get("text") == "INV-SL42" and tag.get("uri") == URI
    assert {"sim": "hid", "text": "INV-SL42"} in out, "the tap types the text record"
    assert len(events(out, "tag_removed")) == 1


def test_a_tag_that_holds_a_message_is_not_overwritten_unless_asked(sim):
    programmed(sim, 42)
    code, out = program(sim, 43)
    assert code == 1 and failure(out) == "not_blank"
    assert events(out, "failed")[0].get("text") == "INV-SL42"
    assert events(out, "failed")[0].get("uri") == URI
    assert not events(out, "writing")


def test_overwrite_and_protect(sim):
    protected(sim)
    out = sim.send("!tap")
    (tag,) = events(out, "tag")
    assert tag.get("text") == "INV-SL43" and tag.get("protected") is True


@pytest.mark.parametrize(
    "args, error",
    [((), "auth_required"), (("--pwd", "00000000"), "auth_failed")],
    ids=["no password", "wrong password"],
)
def test_a_protected_tag_refuses_without_its_password(sim, args, error):
    protected(sim)
    code, out = program(sim, 44, "--overwrite", *args)
    assert code == 1 and failure(out) == error


def test_a_protected_tag_takes_its_password(sim):
    protected(sim)
    code, _ = program(sim, 44, "--overwrite", "--pwd", "A1B2C3D4")
    assert code == 0


def test_wipe(sim):
    protected(sim)
    code, out = sim.nfcprog("wipe")
    assert code == 1 and failure(out) == "auth_required"
    code, out = sim.nfcprog("wipe", "--pwd", "A1B2C3D4")
    assert code == 0 and out[-1].get("protected") is False
    assert "text" not in events(sim.send("!tap"), "tag")[0], "a wiped tag is blank"


def test_a_tag_pulled_away_mid_write_is_left_empty_not_broken(sim):
    sim.send("!tag ntag215", "!tear 3")
    code, out = program(sim, 42)
    assert code == 1 and [o.get("evt") for o in out[1:4]] == ["waiting", "writing", "failed"]
    assert failure(out) == "tag_removed"
    (tag,) = events(sim.send("!tap"), "tag")
    assert "text" not in tag and "error" not in tag
    code, _ = program(sim, 42)
    assert code == 0, "presented again, it takes the same job"


def test_a_tag_of_another_kind_is_refused(sim):
    sim.send("!tag classic")
    code, out = program(sim, 42)
    assert code == 1 and failure(out) == "wrong_tag_type"


def test_a_message_too_large_for_the_tag_is_refused(sim):
    sim.send("!tag ntag213")
    code, out = sim.nfcprog("program", "--host", "a-very-long-host-name-" + "x" * 100 + ".example", "--pk", "42")
    assert code == 1 and failure(out) == "too_large"


def test_with_no_tag_a_job_times_out(sim):
    started = time.monotonic()
    code, out = program(sim, 42, "--timeout", "1")
    took = time.monotonic() - started
    assert code == 1 and failure(out) == "timeout"
    assert 1.0 <= took < 4.0, f"after about a second ({took:.1f}s)"


# One job at a time ---------------------------------------------------------------------------


def start_job(link, job_id):
    link.send(json.dumps({"cmd": "program", "id": job_id, "ndef": NDEF_EMPTY_TEXT}))
    return link.read(until=lambda o: o.get("evt") == "waiting")


def test_a_second_job_is_busy_and_cancel_ends_the_first(sim):
    link = sim.link()
    try:
        start_job(link, 1)
        link.send(json.dumps({"cmd": "program", "id": 2, "ndef": NDEF_EMPTY_TEXT}))
        out = link.read(until=lambda o: o.get("rsp") == "program")
        assert out[-1] == {"rsp": "program", "ok": False, "id": 2, "error": "busy"}
        link.send('{"cmd":"cancel"}')
        out = link.read()
        assert {"rsp": "cancel", "ok": True} in out
        assert {"evt": "failed", "id": 1, "error": "cancelled"} in out
    finally:
        link.close()


def test_cancel_with_no_job(sim):
    out = sim.send('{"cmd":"cancel"}')
    assert out == [{"rsp": "cancel", "ok": False, "error": "no_job"}]


def test_two_tags_at_once(sim):
    link = sim.link()
    try:
        start_job(link, 3)
        link.send("!two")
        assert {"evt": "failed", "id": 3, "error": "multiple_tags"} in link.read()
    finally:
        link.close()


# What the firmware will not take --------------------------------------------------------------


def test_malformed_input_changes_nothing(sim):
    link = sim.link()
    try:
        link.send("this is not json")
        out = link.read()
        assert out and out[0].get("evt") == "error" and out[0]["error"] == "bad_json"
        link.send('{"cmd":"program","id":4,"ndef":"D1010B"}')
        out = link.read()
        assert out and out[0].get("rsp") == "program" and out[0]["error"] == "bad_arg"
        assert out[0]["id"] == 4, "a bad command is answered with its id"
        link.send("x" * 3000)
        assert link.read() == [{"evt": "error", "error": "line_too_long"}]
        link.send('{"cmd":"info"}')
        out = link.read()
        assert out and out[0].get("state") == "idle"
    finally:
        link.close()


def test_with_the_reader_down_a_job_is_nfc_error(sim):
    out = sim.send("!nfc off", json.dumps({"cmd": "program", "id": 5, "ndef": NDEF_EMPTY_TEXT}))
    assert {"rsp": "program", "ok": False, "id": 5, "error": "nfc_error"} in out


def test_with_hid_off_a_tap_is_reported_not_typed(sim):
    code, out = sim.nfcprog("hid", "off")
    assert code == 0 and out[-1] == {"rsp": "hid", "ok": True, "enabled": False}
    programmed(sim, 42)
    out = sim.send("!tap")
    assert events(out, "tag")[0].get("text") == "INV-SL42"
    assert not [o for o in out if o.get("sim") == "hid"]


def test_bootloader_is_acknowledged(sim):
    code, out = sim.nfcprog("bootloader")
    assert code == 0 and out[-1] == {"rsp": "bootloader", "ok": True}


# Firmware updates over USB ------------------------------------------------------------------


@pytest.fixture
def image(tmp_path):
    """An image the simulator takes: it restarts reporting the version the first line names."""
    data = b"fw=0.3.0-sim\n" + os.urandom(5000)
    path = tmp_path / "image.bin"
    path.write_bytes(data)
    return data, path


def test_an_image_sent_over_usb_is_taken_and_restarted_into(sim, image):
    _, path = image
    code, out = sim.nfcprog("update", str(path))
    assert code == 0 and [o.get("state") for o in events(out, "ota")] == ["downloading", "restarting"]
    code, out = sim.nfcprog("info")
    assert out[-1].get("fw") == "0.3.0-sim"


def answer_to(name):
    return lambda o: o.get("rsp") == name


def test_an_image_that_is_not_the_one_named_is_refused(sim, image):
    data, _ = image
    link = sim.link()
    try:
        link.send(json.dumps({"cmd": "ota_begin", "id": 9, "size": len(data), "sha256": "00" * 32}))
        link.read(3, until=answer_to("ota_begin"))
        for at in range(0, len(data), 768):
            piece = base64.b64encode(data[at : at + 768]).decode()
            link.send(json.dumps({"cmd": "ota_data", "id": 9, "at": at, "data": piece}))
            link.read(3, until=answer_to("ota_data"))
        link.send('{"cmd":"ota_end","id":9}')
        out = link.read(3, until=lambda o: o.get("evt") == "ota" and o.get("state") == "failed")
        out += link.read(0.2)
    finally:
        link.close()
    assert {"rsp": "ota_end", "ok": False, "id": 9, "error": "verify_failed", "detail": "sha256 does not match"} in out
    assert {"evt": "ota", "state": "failed", "error": "verify_failed", "detail": "sha256 does not match"} in out
    code, out = sim.nfcprog("info")
    assert out[-1].get("state") == "idle", "nothing restarted"


def test_no_job_during_an_update_and_a_piece_out_of_order_ends_it(sim, image):
    data, _ = image
    fw = sim.nfcprog("info")[1][-1]["fw"]
    link = sim.link()
    try:
        digest = hashlib.sha256(data).hexdigest()
        link.send(json.dumps({"cmd": "ota_begin", "id": 10, "size": len(data), "sha256": digest}))
        link.read(3, until=answer_to("ota_begin"))
        link.send(json.dumps({"cmd": "program", "id": 11, "ndef": NDEF_EMPTY_TEXT}))
        out = link.read(3, until=answer_to("program"))
        assert {"rsp": "program", "ok": False, "id": 11, "error": "busy",
                "detail": "a firmware update is in progress"} in out
        piece = base64.b64encode(data[:768]).decode()
        link.send(json.dumps({"cmd": "ota_data", "id": 10, "at": 768, "data": piece}))
        out = link.read(3, until=answer_to("ota_data"))
        assert {"rsp": "ota_data", "ok": False, "id": 10, "error": "bad_arg", "detail": "at: expected 0"} in out
        assert {"evt": "ota", "state": "failed", "error": "bad_arg", "detail": "at: expected 0"} in out
        link.send('{"cmd":"ota_end","id":10}')
        out = link.read(3, until=answer_to("ota_end"))
        assert out and out[0].get("error") == "no_job", "nothing is left of it"
    finally:
        link.close()
    code, out = sim.nfcprog("info")
    assert out[-1].get("fw") == fw and out[-1].get("state") == "idle"


def test_the_simulator_reports_the_version_in_version_txt(sim):
    _, out = sim.nfcprog("info")
    assert out[-1]["fw"] == (TOOLS.parent / "version.txt").read_text().strip()


# The WebSerial page's NDEF builder agrees with nfcprog.py's -----------------------------------

NODE_NDEF = """
const fs = require('fs');
const src = fs.readFileSync(process.argv[1], 'utf8').match(/<script>([\\s\\S]*)<\\/script>/)[1];
const m = { exports: {} };
new Function('module', src)(m);
const [host, pk, scheme] = process.argv.slice(2);
console.log(m.exports.toHex(m.exports.buildNdef(host, Number(pk), scheme)));
"""


@pytest.mark.parametrize(
    "host, pk, scheme", [(HOST, 42, "https"), ("10.0.0.5:8000", 7, "http"), ("a.b", 123456, "https")]
)
def test_the_page_and_nfcprog_build_the_same_ndef(host, pk, scheme):
    from nfcprog import build_ndef

    node = shutil.which("node")
    if not node:
        pytest.skip("node is not installed")
    page = TOOLS / "webserial.html"
    js = subprocess.run([node, "-e", NODE_NDEF, str(page), host, str(pk), scheme],
                        capture_output=True, text=True, timeout=30).stdout.strip()
    assert js == build_ndef(host, pk, scheme).hex().upper()
