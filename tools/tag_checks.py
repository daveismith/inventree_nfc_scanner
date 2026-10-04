#!/usr/bin/env python3
"""Guided checks of the scanner with real tags.

Run it in a terminal with the scanner plugged in and the tags from docs/test-tags.md to hand.
It tells you which tag to present and when, sends the jobs, and checks what the scanner says:

    python tools/tag_checks.py --host inventree.example --pk 42

`--pk` should be a real stock location, so the tag left at the end opens a real page. Jobs
that are only there to be refused or overwritten use pk + 1000.

Needs two blank NTAG215 tags, plus (each step can be skipped) the phone-erased tag, a tag
that already holds a message, a card that is not an NTAG, and any two tags. Every line
exchanged is written to tag_checks.log, which is the thing to keep if a step fails.

Nothing here writes a tag's permanent lock bits. The password steps use A1B2C3D4 and remove
it again before they finish.
"""

import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nfcprog import Device, build_ndef, find_port  # noqa: E402

PWD, PACK, WRONG_PWD = 'A1B2C3D4', '1234', '00000000'
WAIT_S = 90             # how long to wait for a person to do something

BOLD, GREEN, RED, YELLOW, RESET = '\033[1m', '\033[32m', '\033[31m', '\033[33m', '\033[0m'


class Checks:
    def __init__(self, dev, log, host, pk):
        self.dev = dev
        self.log = log
        self.host = host
        self.pk = pk
        self.scratch_pk = pk + 1000
        self.next_id = 1
        self.passed = self.failed = self.skipped = 0
        self.t0 = time.monotonic()

    # --- plumbing

    def record(self, direction, obj):
        self.log.write(f'{time.monotonic() - self.t0:8.2f} {direction} {json.dumps(obj, separators=(",", ":"))}\n')
        self.log.flush()

    def send(self, obj):
        self.record('>', obj)
        self.dev.send(obj)

    def lines(self, seconds):
        for msg in self.dev.lines(seconds):
            self.record('<', msg)
            yield msg

    def wait_for(self, seconds, wanted):
        """The first message `wanted` accepts, with everything seen on the way; or None."""
        seen = []
        for msg in self.lines(seconds):
            seen.append(msg)
            if wanted(msg):
                return msg, seen
        return None, seen

    def info(self):
        self.send({'cmd': 'info'})
        msg, _ = self.wait_for(3, lambda m: m.get('rsp') == 'info')
        return msg or {}

    # --- talking to the person

    def say(self, text):
        print(f'\n{BOLD}>> {text}{RESET}', flush=True)
        self.log.write(f'{time.monotonic() - self.t0:8.2f} # {text}\n')

    def result(self, ok, what, got=None):
        if ok:
            self.passed += 1
            print(f'   {GREEN}pass{RESET}  {what}', flush=True)
        else:
            self.failed += 1
            print(f'   {RED}FAIL{RESET}  {what}', flush=True)
            if got is not None:
                print(f'         got: {json.dumps(got, separators=(",", ":"))}', flush=True)
        self.log.write(f'{time.monotonic() - self.t0:8.2f} # {"pass" if ok else "FAIL"}: {what}\n')
        return ok

    def skip(self, what):
        self.skipped += 1
        print(f'   {YELLOW}skip{RESET}  {what}', flush=True)
        self.log.write(f'{time.monotonic() - self.t0:8.2f} # skip: {what}\n')

    def have(self, question):
        answer = input(f'\n{BOLD}?? {question} [Y/n] {RESET}').strip().lower()
        return answer in ('', 'y', 'yes')

    # --- the reader

    def clear_reader(self):
        """Ask for the reader to be emptied, and wait until the scanner agrees it is."""
        if self.info().get('tag') is None:
            return
        self.say('Take the tag off the reader.')
        deadline = time.monotonic() + WAIT_S
        while time.monotonic() < deadline:
            for _ in self.lines(0.5):
                pass
            if self.info().get('tag') is None:
                return
        self.result(False, 'the reader is clear')

    def tap(self, prompt):
        """Ask for a tag and return the `tag` event it produces."""
        self.say(prompt)
        msg, _ = self.wait_for(WAIT_S, lambda m: m.get('evt') == 'tag')
        return msg or {}

    def job(self, cmd, timeout_s=10, on_writing=None):
        """Send a job; return its final event (done or failed) and everything before it."""
        cmd = dict(cmd, id=self.next_id, timeout_ms=int(timeout_s * 1000))
        self.next_id += 1
        self.send(cmd)
        seen = []
        for msg in self.lines(timeout_s + 5):
            seen.append(msg)
            if msg.get('rsp') == cmd['cmd'] and not msg.get('ok'):
                return msg, seen
            if msg.get('evt') == 'writing' and on_writing:
                on_writing()
            if msg.get('id') == cmd['id'] and msg.get('evt') in ('done', 'failed'):
                return msg, seen
        return {}, seen

    def program(self, pk, **extra):
        on_writing = extra.pop('on_writing', None)
        timeout_s = extra.pop('timeout_s', 10)
        cmd = {'cmd': 'program', 'ndef': build_ndef(self.host, pk).hex().upper()}
        cmd.update(extra)
        return self.job(cmd, timeout_s, on_writing)

    def expect_done(self, final, seen, what, protected):
        order = [m.get('evt') for m in seen if m.get('evt')]
        ok = final.get('evt') == 'done' and final.get('protected') is protected and 'writing' in order
        return self.result(ok, what, final or seen)

    def expect_failed(self, final, error, what, wrote=False, seen=()):
        ok = final.get('error') == error and final.get('evt', 'failed') == 'failed'
        if ok and not wrote and any(m.get('evt') == 'writing' for m in seen):
            ok = False                  # refused, but only after starting to write
        return self.result(ok, what, final or list(seen))

    # --- the checks

    def blank_tag_life_cycle(self):
        text, scratch = f'INV-SL{self.pk}', f'INV-SL{self.scratch_pk}'
        uri = f'https://{self.host}/web/stock/location/{self.pk}'

        self.clear_reader()
        tag = self.tap('Put a BLANK tag on the reader and leave it there.')
        self.result(tag.get('type') == 'ntag215' and 'text' not in tag and tag.get('protected') is False,
                    'blank tag reported: ntag215, no text, unprotected', tag)
        uid = tag.get('uid')
        self.result(self.info().get('tag') == uid, 'info names the tag on the reader')

        final, seen = self.program(self.scratch_pk)
        self.expect_done(final, seen, 'program a blank tag: waiting, writing, done', protected=False)
        self.result(final.get('uid') == uid, 'done carries the tag\'s UID', final)

        final, seen = self.program(self.pk)
        self.expect_failed(final, 'not_blank', 'a second program is refused: not_blank', seen=seen)
        self.result(final.get('text') == scratch, f'and says what is there ({scratch})', final)

        final, seen = self.program(self.pk, overwrite=True, pwd=PWD, pack=PACK)
        self.expect_done(final, seen, 'overwrite and protect with a password', protected=True)

        final, seen = self.program(self.scratch_pk, overwrite=True)
        self.expect_failed(final, 'auth_required', 'overwrite with no password: auth_required', seen=seen)
        final, seen = self.program(self.scratch_pk, overwrite=True, pwd=WRONG_PWD)
        self.expect_failed(final, 'auth_failed', 'overwrite with the wrong password: auth_failed', seen=seen)
        final, seen = self.program(self.scratch_pk, overwrite=True, pwd=PWD, pack=PACK)
        self.expect_done(final, seen, 'overwrite with the right password', protected=True)

        final, seen = self.job({'cmd': 'wipe'})
        self.expect_failed(final, 'auth_required', 'wipe with no password: auth_required', seen=seen)
        final, seen = self.job({'cmd': 'wipe', 'pwd': PWD})
        self.expect_done(final, seen, 'wipe with the password: unprotected again', protected=False)

        final, seen = self.program(self.pk)
        self.expect_done(final, seen, 'program the wiped tag without overwrite', protected=False)

        self.say('Lift the tag, wait a second, and put it back.')
        gone, _ = self.wait_for(WAIT_S, lambda m: m.get('evt') == 'tag_removed')
        self.result(gone is not None, 'removal reported', gone)
        back, _ = self.wait_for(WAIT_S, lambda m: m.get('evt') == 'tag')
        back = back or {}
        self.result(back.get('text') == text and back.get('uri') == uri and back.get('protected') is False,
                    f'read back: {text} and its URL', back)
        print(f'   This tag now holds {text}. Later: check that a phone opens {uri}')

    def torn_write(self):
        self.clear_reader()
        tag = self.tap('Put a SECOND blank tag on the reader. Be ready to lift it when told.')
        if 'text' in tag or tag.get('type') != 'ntag215':
            self.result(False, 'a blank NTAG215 for the pulled-away test', tag)
            return

        # A long message, so there is time to pull the tag away while it is being written.
        filler = ''.join(chr(ord('a') + i % 26) for i in range(380))
        payload = b'\x02en' + filler.encode()
        ndef = bytes([0xC1, 0x01]) + len(payload).to_bytes(4, 'big') + b'T' + payload

        def lift_now():
            print(f'\n{BOLD}{RED}>> LIFT THE TAG NOW{RESET}', flush=True)

        final, seen = self.job({'cmd': 'program', 'ndef': ndef.hex().upper()}, on_writing=lift_now)
        if final.get('evt') == 'done':
            self.skip('the write finished before the tag was lifted; nothing was torn')
            self.program(self.scratch_pk, overwrite=True)
            return
        self.expect_failed(final, 'tag_removed', 'pulled away mid-write: tag_removed', wrote=True, seen=seen)

        self.clear_reader()
        tag = self.tap('Put that same tag back.')
        self.result(tag.get('type') == 'ntag215' and 'text' not in tag and 'error' not in tag,
                    'the torn tag reads as blank, not broken', tag)
        final, seen = self.program(self.scratch_pk)
        self.expect_done(final, seen, 'and takes a new job without overwrite', protected=False)

    def phone_erased(self):
        self.clear_reader()
        tag = self.tap('Put the tag you erased with NFC Tools (tag 6) on the reader.')
        self.result('text' not in tag and 'uri' not in tag, 'the erased tag shows no records', tag)
        final, seen = self.program(self.scratch_pk)
        self.expect_done(final, seen, 'a phone-erased tag counts as blank: programmed without overwrite',
                         protected=False)

    def phone_written(self):
        self.clear_reader()
        tag = self.tap('Put a tag you wrote with NFC Tools (tag 1) on the reader.')
        self.result(bool(tag.get('text')), 'its text record is reported', tag)
        final, seen = self.program(self.scratch_pk)
        self.expect_failed(final, 'not_blank', 'it is not overwritten: not_blank', seen=seen)
        self.result(final.get('text') == tag.get('text'), 'and its own text is reported back', final)

    def wrong_type(self):
        self.clear_reader()
        self.say('Present the card that is not an NTAG.')
        final, seen = self.program(self.scratch_pk, timeout_s=WAIT_S)
        self.expect_failed(final, 'wrong_tag_type', 'a job on another kind of card: wrong_tag_type', seen=seen)

    def two_tags(self):
        # Tags that already hold a message, and no overwrite: whichever way it goes, nothing
        # is written. If one tag is seen a moment before the other, the answer is not_blank.
        for attempt in range(3):
            self.clear_reader()
            self.say('Present TWO written tags together, stacked, in one movement.')
            final, seen = self.program(self.scratch_pk, timeout_s=WAIT_S)
            if final.get('error') != 'not_blank':
                break
            print('   One tag was seen before the other. Again, more together.')
        self.expect_failed(final, 'multiple_tags', 'two tags at once: multiple_tags', seen=seen)

    def timeout(self):
        self.clear_reader()
        started = time.monotonic()
        final, seen = self.program(self.scratch_pk, timeout_s=3)
        took = time.monotonic() - started
        self.expect_failed(final, 'timeout', f'no tag: timeout after {took:.1f}s (asked for 3)', seen=seen)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--host', required=True, help='InvenTree host name')
    ap.add_argument('--pk', required=True, type=int, help='a real stock location\'s primary key')
    ap.add_argument('--port', help='serial port; found by USB ID when omitted')
    ap.add_argument('--log', default='tag_checks.log', help='where every line exchanged is written')
    args = ap.parse_args()

    dev = Device(args.port or find_port())
    with open(args.log, 'w') as log:
        c = Checks(dev, log, args.host, args.pk)
        try:
            info = c.info()
            if not info.get('pn532'):
                sys.exit(f'the scanner has no working reader: {info}')
            print(f'Scanner fw {info.get("fw")}, PN532 {info["pn532"]["ver"]}. Log: {args.log}')
            if info.get('hid'):
                # Otherwise every read-back would be typed into this terminal.
                c.send({'cmd': 'hid', 'enabled': False})
                c.wait_for(2, lambda m: m.get('rsp') == 'hid')
                print('Keyboard output is off for this session.')

            c.timeout()
            c.blank_tag_life_cycle()
            c.torn_write()
            for question, check in (
                ('Do you have the phone-erased tag (tag 6)?', c.phone_erased),
                ('Do you have a tag written with NFC Tools (tag 1)?', c.phone_written),
                ('Do you have a card that is not an NTAG?', c.wrong_type),
                ('Do you have two written tags to present together?', c.two_tags),
            ):
                if c.have(question):
                    check()
                else:
                    c.skip(question)
        except KeyboardInterrupt:
            print('\ninterrupted')
        finally:
            dev.close()

        colour = RED if c.failed else GREEN
        print(f'\n{colour}{c.passed} passed, {c.failed} failed, {c.skipped} skipped{RESET}. Log: {args.log}')
        return 1 if c.failed else 0


if __name__ == '__main__':
    sys.exit(main())
