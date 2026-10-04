# Test tags

The set of NFC tags used to check the scanner on real hardware, and what each one should
make it do. The host tests (`host_test/`, `tools/test_sim.py`) cover the same behaviour
against a simulated tag; this is the part only real tags and a real PN532 can confirm.

Run on the board on 2026-10-04: every tag in the first table read as expected, all 26 checks
of the guided script passed, and a phone opened the location page from a tag the scanner
wrote. Not yet run: the keyboard check, and the two optional tags at the end. When a tag
behaves differently from the "Expect" column, that is a finding: note what the scanner
sent, and what the phone shows under NFC Tools → Read → memory.

Throughout, `<host>` is the InvenTree host name and `42` a real stock location's primary key,
so that the phone and keyboard checks open a real page. Number the tags with a marker.

## Tags written with a phone

Written with the NFC Tools app (Write → Add a record), records in the order given. Leave a
Text record's language at the app's default.

| # | Records | Expect on a tap | Why it is in the set |
| --- | --- | --- | --- |
| 1 | URL `https://<host>/web/stock/location/42`, then Text `INV-SL42` | `tag` event with both `text` and `uri`; types `INV-SL42` and Enter | The real format, written by something other than this firmware |
| 2 | Text `INV-SL42`, then URL as above | Same as tag 1 | Record order must not matter |
| 3 | Text `INV-SL7` only | `tag` with `text`, no `uri`; types `INV-SL7` | A text record alone is enough to type |
| 4 | URL as above, only | `tag` with `uri`, no `text`; types nothing | Nothing is typed without a text record |
| 5 | The two records of tag 1, then a third Text record of about 300 characters of filler | Same as tag 1 | A message over 255 bytes uses the long TLV length and a long-form record |
| 6 | Anything, then **Erase tag** | `tag` with neither `text` nor `uri` | A phone-erased tag must count as blank (see below) |

Then, with a job:

- **Tags 1 to 5**, `nfcprog.py program --host <host> --pk 43`: `failed` with `not_blank`, and
  the `text` and `uri` already on the tag. No `writing` event. With `--overwrite`: `done`.
- **Tag 6**, the same command without `--overwrite`: `done`. A phone is expected to erase by
  writing one Empty record rather than a zero-length message, and the firmware treats that
  as blank. If this reports `not_blank` instead, NFC Tools writes something else, and the
  tag's memory dump is what is needed to fix it.

## The guided script

`tools/tag_checks.py` runs the checks below that involve a job, telling you which tag to
present and when, and checking what the scanner answers:

```sh
python tools/tag_checks.py --host <host> --pk 42
```

It needs two blank tags, and asks before each step that uses one of the others. Everything
exchanged goes to `tag_checks.log`.

## Keyboard output

`tools/hid_check.py` turns keyboard output on for one session, asks for a tap, and reads what
the scanner types back from the terminal it runs in, so the keystrokes land somewhere safe
and are compared with the tag's text record. Use a tag with a text record (tags 1, 2, 3 or 5).

## Tags left as they come

| Tags | Used for |
| --- | --- |
| Four or more factory-blank NTAG215 | A plain program; a program with a password; one pulled away mid-write; a spare |
| Any card that is not an NTAG21x: a hotel key, a transit card, a MIFARE Classic fob | `wrong_tag_type` |

What to do with them:

| Check | How | Expect |
| --- | --- | --- |
| Blank tag, tapped | Tap | `tag` with `type` `ntag215`, `protected` false, no `text` |
| Program | `nfcprog.py program --host <host> --pk 42`, then tap | `waiting`, `writing`, `done` with the UID. The phone opens the location page; NFC Tools shows the two records |
| Program with a password | Add `--pwd A1B2C3D4` | `done` with `protected` true. The phone still reads it, and NFC Tools cannot write it |
| Re-program a protected tag | `--overwrite` with no password, a wrong one, then the right one | `auth_required`, `auth_failed`, `done` |
| Remove protection | `nfcprog.py wipe --pwd A1B2C3D4` | `done` with `protected` false; the tag is blank and open again |
| Pulled away mid-write | Start a program job and lift the tag as the LED starts blinking blue | `failed` with `tag_removed`. Tapped again it reads as blank, not broken, and takes the same job |
| Not an NTAG | Tap the other card; then present it to a waiting job | `tag` with `type` `unknown`; the job fails with `wrong_tag_type` |
| Two at once | Present two tags together, idle and then to a waiting job | `tag` with `error` `multiple_tags`; the job fails with `multiple_tags` |
| No tag | `nfcprog.py program ... --timeout 5`, present nothing | `failed` with `timeout` after five seconds |

## Optional

**A tag given a password in NFC Tools** (Other → Set password). How the app turns the
password into the tag's four bytes, and which pages it protects, is not known, so the
scanner is expected to see a protected tag it cannot open: `auth_required` without a
password and `auth_failed` with one. If the app also protects reading, a tap reports the tag
with no `text`. Keep the password so the app can remove it afterwards.

**One tag permanently locked with NFC Tools' "Lock tag".** This cannot be undone and the
tag can never be written again. It is the only way to see the `locked` error on a real tag:
a program job should fail with `locked` and write nothing.
