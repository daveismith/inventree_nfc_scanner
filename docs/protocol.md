# Serial protocol

The scanner is driven by lines of JSON over its USB CDC port. The same objects travel over
the network inside the plugin's `sync/` exchange (see [Over the network](#over-the-network)).
This document is the contract: a host built from it should work without reading the
firmware. Where it and the code disagree, the code is `components/proto` (lines to structs
and back) and `components/app_core` (what the device does with them), and one of the two
is a bug.

Protocol version: **1**.

[protocol.schema.json](protocol.schema.json) is the same contract as a JSON Schema (draft
2020-12): `#/$defs/command` for a line to the device, `#/$defs/message` for a line from it.
The simulator tests hold every line the device sends, over USB and over the network, to it.

## Lines

- Each line is one JSON object, UTF-8, ended by `\n`. A `\r` before the `\n` is dropped, so
  `\r\n` works too. Empty lines are ignored. The device ends its lines with `\n` alone.
- A line the device receives may be at most 2047 bytes before the `\n`. A longer one is
  thrown away up to its `\n` and answered with an `error` event, `line_too_long`. Bytes lost
  on the way in (the receive buffer overflowing) are reported the same way.
- The baud rate and other line settings mean nothing to a CDC port and are ignored.
- Field names are case-sensitive. Fields the device does not know are ignored.
- Hex is output in upper case and accepted in either case.
- Numbers called *integers* below must be whole numbers within the range given; `7.0` is
  accepted, `7.5` is not.

### Sessions

A session is the port being open: the host raising DTR. About 200 ms after DTR rises the
device sends `hello` on that link. Closing the port (DTR falling, or the cable pulled) ends
the session, and with it:

- a job the session started is cancelled (`failed` with `cancelled`, to any other link
  that would see it),
- a `hid` setting made without `persist` lapses,
- an update being sent with `ota_begin` is abandoned (`ota` event, `failed`).

A host should wait for `hello`, or send `info`, before relying on the device being ready.

## Commands and answers

Every command is an object with `cmd`, the command's name. It gets exactly one immediate
answer, a `rsp`, on the link it came from:

```json
{"rsp":"program","ok":true,"id":7}
{"rsp":"program","ok":false,"id":7,"error":"busy"}
{"rsp":"program","ok":false,"id":7,"error":"bad_arg","detail":"pwd: expected 8 hex digits"}
```

| Field | |
| --- | --- |
| `rsp` | The command's name, as sent |
| `ok` | Whether the command was accepted |
| `id` | The command's `id`, when it had one |
| `error` | When `ok` is false: one of the [error codes](#errors) |
| `detail` | Optional: a short human-readable explanation, for a log or a person, not to be parsed |

Some answers carry more fields; they are listed with their commands.

`id` is an integer from 0 to 2147483647 chosen by the host. It is required on `program`,
`wipe` and the `ota_` commands, and optional on the rest, where it is only echoed back.
Jobs carry it in their events.

A line that is a JSON object with a `cmd` string is always answered as a `rsp` under that
name, even when the command is unknown (`unknown_cmd`; a name over 23 bytes is cut in the
answer) or its fields are wrong (`bad_arg`). Anything else, a line that is not JSON, not an
object or has no `cmd`, is answered with an `error` event, since there is no command to
answer. If the device cannot take a command within two seconds, it answers `busy`.

### `info`

What the device is and what it is doing.

```json
{"cmd":"info"}
{"rsp":"info","ok":true,"proto":1,"fw":"1.0.0","reader":"nfc-34b7da52a084","idf":"v6.1",
 "pn532":{"ic":50,"ver":"1.6"},"state":"idle","job":null,"tag":null,"hid":true,
 "buzzer":true,"reset":"poweron","crash":null,"uptime_ms":81234,"net":null}
```

(Shown on two lines here; on the wire it is one.)

| Field | |
| --- | --- |
| `proto` | The protocol version: 1 |
| `fw` | The firmware version |
| `reader` | The scanner's id: `nfc-` and its MAC address in lower-case hex. The same on USB and over the network |
| `idf` | The ESP-IDF version it was built with |
| `pn532` | The NFC chip's `ic` (a number) and firmware `ver` (`"major.minor"`), or `null` when it is not answering |
| `state` | `idle`, `waiting` (a job is waiting for a tag) or `nfc_error` (the NFC chip is not answering) |
| `job` | The id of the job waiting, or `null` |
| `tag` | The UID of the tag on the reader now (hex), or `null` |
| `hid` | Whether a tap types its text now |
| `buzzer` | Whether the board has a buzzer |
| `reset` | Why the chip last reset: `poweron`, `external`, `software`, `panic`, `int_wdt`, `task_wdt`, `wdt`, `deepsleep`, `brownout`, `usb`, `jtag`, `power_glitch`, `cpu_lockup` or `unknown` |
| `crash` | One line about the last crash, or `null` |
| `uptime_ms` | Milliseconds since start (wraps after about 49 days) |
| `net` | In a network build, the [network status](#network-status); `null` otherwise |

### `program`

Start a job: write an NDEF message to the next tag presented.

| Field | |
| --- | --- |
| `id` | Required. The job's id |
| `ndef` | Required. The whole NDEF message, as hex: at most 888 bytes (1776 hex digits), a complete, valid message, with any text record in UTF-8. The device wraps it in its TLV |
| `overwrite` | `true` to write over a tag that already holds data. Without it such a tag fails the job with `not_blank` |
| `pwd` | 8 hex digits: protect the tag with this password |
| `pack` | 4 hex digits: the password acknowledgement to set with `pwd`. Only with `pwd` |
| `old_pwd` | 8 hex digits: the password the tag is protected with now, when changing it to `pwd`. Without it, `pwd` is also tried as the tag's current password |
| `timeout_ms` | How long to wait for a tag: an integer from 1000 to 600000. Default 60000 |

The `rsp` says whether the job started. It fails with `busy` while another job is waiting
or a firmware update is running, and with `nfc_error` while the NFC chip is not answering.
On success, a [job's events](#job-events) follow.

### `wipe`

Start a job: empty the next tag presented, leaving an empty NDEF message, and remove its
password.

| Field | |
| --- | --- |
| `id` | Required. The job's id |
| `pwd` | 8 hex digits: the tag's password, if it has one |
| `timeout_ms` | As for `program` |

Answered and followed as `program` is.

### `cancel`

End the job waiting.

| Field | |
| --- | --- |
| `id` | Optional: cancel only if this is the job waiting |

`no_job` when there is no job waiting, or it has another id. Otherwise `ok`, followed by
the job's `failed` with `cancelled`.

### `hid`

Whether a tap types its text as a USB keyboard. Only text that looks like an InvenTree
barcode (the build's prefix, `INV-` by default, two capital letters and one to ten digits)
is ever typed, unless the firmware was built with `CONFIG_APP_HID_TYPE_ANY`; nothing is
typed while a job is waiting, or by a build without the keyboard.

| Field | |
| --- | --- |
| `enabled` | Required. `true` or `false` |
| `persist` | `true` to make it the setting the device starts with. Without it the setting lasts until this link's session ends |

The answer carries `enabled`: whether a tap types now.

### `log`

Forward the firmware's log to this port as `log` events.

| Field | |
| --- | --- |
| `level` | Required. `off`, `error`, `warn`, `info` or `debug` |

The level starts as `off` and lasts until the device restarts; closing the port does not
reset it. `debug` turns on the firmware's debug logging too; the API token is kept out of
it.

### `bootloader`

Restart into the chip's ROM download mode, for flashing with esptool. Answered `ok` first;
a job waiting is cancelled; then the port goes away and the chip's own download port
appears.

### `debug`

Development builds only (`CONFIG_APP_DEV_RECOVERY`): provoke a failure on purpose, to test
recovery. `unknown_cmd` in other builds.

| Field | |
| --- | --- |
| `action` | Required. `crash` (panic), `hang` (stop, for the watchdog to find) or `nousb` (restart and stay off USB on the next boot) |

Answered `ok` before the action, which may not return.

### `net`

Network builds only (`CONFIG_APP_NET_ENABLE`); `unknown_cmd` in others. Configures the
Wi-Fi and the plugin the scanner reports to. Every form is answered with the
[network status](#network-status), after the change. Changes are saved and last across
restarts.

| Form | |
| --- | --- |
| `{"cmd":"net"}` | Report only |
| `{"cmd":"net","action":"join","ssid":"…","psk":"…"}` | Remember a network and prefer it. `ssid` 1 to 32 bytes; `psk` 8 to 64 characters, or empty or absent for an open network. At most four networks are kept: a fifth is `bad_arg` |
| `{"cmd":"net","action":"forget","ssid":"…"}` | Forget a network. `bad_arg` if it is not kept |
| `{"cmd":"net","action":"server","url":"…","token":"…"}` | The plugin's base URL (up to 160 bytes, e.g. `https://inventree.example/plugin/nfcscanner`) and an InvenTree API token (up to 96 bytes). `https://` only, with no user info, unless the build has `CONFIG_APP_NET_ALLOW_HTTP`. `token` is optional: without it the token is kept if the URL is on the same server, and cleared if not |
| `{"cmd":"net","action":"poll","poll_ms":1000,"wait_s":25}` | Pacing, either or both: `poll_ms` 100 to 60000, how often to call when idle; `wait_s` 0 to 300, how long to ask the server to hold a call (0: plain polling). Defaults 1000 and 25 |
| `"enabled": true` or `false` | With any form, or alone: switch the network side on or off |

The passphrase and the token are never sent back, in any answer or event.

### `ota`

Network builds only. Fetch a firmware image over HTTP and install it.

| Field | |
| --- | --- |
| `url` | Required. Up to 256 bytes, `https://` only with no user info (http as for `net`). Over the network, it must be on the plugin's own server |
| `sha256` | Required. The image's SHA-256, 64 hex digits |

`ok` means the download has started; `busy` while a job is waiting or another update is
running. Its progress comes as `ota` events: `downloading`, then `restarting` (once any job
running has ended) or `failed`. The new image boots on trial and is rolled back if it does
not come up healthy.

### `ota_begin`, `ota_data`, `ota_end`

Any build: send a firmware image over the serial port itself.

| Command | Fields |
| --- | --- |
| `ota_begin` | `id` (required, names this update), `size` (required, the image's length in bytes: 1 to 16777216, and no more than the application slot), `sha256` (required, 64 hex digits) |
| `ota_data` | `id`, `at` (required: the offset this piece goes at, which must be where the last piece ended, so 0 first), `data` (required: standard base64 with padding, of 1 to 768 bytes) |
| `ota_end` | `id` |

Each line is answered; send the next piece after the answer to the last. The rules:

- `ota_begin` fails with `busy` while a job is waiting, while an update over the network
  runs, or while another link is sending one (until it has been quiet for 30 seconds).
  A second `ota_begin` from the same link abandons the first.
- The first piece must start a valid image for this chip, or `write_failed`.
- An `ota_data` or `ota_end` whose `id` is not the update in progress is `no_job`.
- A piece at the wrong offset, or one that goes past `size`, is `bad_arg` and ends the
  update. So does `ota_end` before `size` bytes have arrived.
- `ota_end` checks the digest and the image: `verify_failed` if either is wrong. Otherwise it
  answers `ok`, sends `ota` `restarting` and restarts into the image, on trial as above.
- The update is abandoned if the port closes, or nothing arrives for 30 seconds, and an
  `ota` event `failed` says so. Any failure that ends the update sends one too.

`ota_begin` sends an `ota` event, `downloading`.

## Events

Events report what happens, as it happens, and are not answers to anything. Each has
`evt`, its name.

| Event | Fields | Goes to |
| --- | --- | --- |
| `hello` | `proto`, `fw`, `reader` (as in `info`) | The link that just opened |
| `waiting` | `id`, `timeout_ms` | [Job events](#job-events) |
| `writing` | `id`, `uid` | Job events |
| `done` | `id`, `uid`, `type`, `protected` | Job events |
| `failed` | `id`, `error`, and maybe `detail`, `uid`, `text`, `uri` | Job events |
| `tag` | `uid`, `type`, and maybe `text`, `uri`, `protected`, `error` | Every link |
| `tag_removed` | `uid` | Every link |
| `error` | `error`, and maybe `detail` | The link that sent the line |
| `log` | `lvl`, and maybe `tag`, `msg` | USB |
| `net` | The [network status](#network-status) | USB |
| `ota` | `state`, and when it failed `error`, and maybe `detail` | Every link |

Field types:

- `uid`: the tag's UID in hex, 14 digits for the NTAG21x tags this device writes.
- `type`: `ntag213`, `ntag215`, `ntag216` or `unknown`.
- `protected`: whether the tag is password protected.
- `text`, `uri`: the first text record and the first URI record of the tag's NDEF message,
  the URI with its prefix expanded. A URI over 255 characters once expanded is not reported.
  If a line would grow past the limit with them, both are left out and `detail` says so.

### Taps

With no job waiting, a tag presented to the reader is read and reported once as `tag`,
until it is taken away (`tag_removed`). A tag that is not an NTAG21x is reported with
`type` `unknown` and nothing else. Two or more tags at once give a `tag` with
`error` `multiple_tags` and no `uid`. A tag pulled away before it could be read is not
reported.

```json
{"evt":"tag","uid":"04A1B2C3D4E5F6","type":"ntag215","text":"INV-SL4","uri":"https://inventree.example/web/stock/location/4","protected":true}
{"evt":"tag_removed","uid":"04A1B2C3D4E5F6"}
```

### Job events

`program` and `wipe` are jobs. One job runs at a time. A job's events are:

```json
{"evt":"waiting","id":7,"timeout_ms":60000}
{"evt":"writing","id":7,"uid":"04A1B2C3D4E5F6"}
{"evt":"done","id":7,"uid":"04A1B2C3D4E5F6","type":"ntag215","protected":true}
```

- `waiting` follows the `rsp` at once. A tag already on the reader is used straight away;
  otherwise the first tag presented decides the job. Taps are not reported, or typed,
  while a job waits.
- `writing` is sent when the device starts to change the tag. A failure before it (the
  wrong tag, a password refused) has left the tag untouched.
- Every job ends with exactly one `done` or `failed`. `failed` carries the reason in
  `error`, and the tag's `uid` when there was one. With `not_blank` it also carries the
  `text` and `uri` already on the tag, so the host can ask before sending the job again with
  `overwrite`.
- A tag pulled away mid-write is left holding an empty, valid message.

Who sees them: a job started over USB has its events sent to USB only. A job started by
the plugin over the network has them sent to every link, so a USB page sees the server's
jobs, but the server never hears of jobs it did not start.

### `error`

A line that could not be read as a command: `bad_json` (not JSON, not an object, or no
`cmd` string) or `line_too_long`.

```json
{"evt":"error","error":"bad_json","detail":"expected one JSON object"}
```

### `log`

A line of the firmware's log, once `log` has set a level.

```json
{"evt":"log","lvl":"I","tag":"net","msg":"..."}
```

`lvl` is `E`, `W`, `I`, `D` or `V`, or `?` for a line that is not in the usual form, whose
whole text is then `msg` and which has no `tag`. Log lines may be dropped when the device is
busy logging; they are for people.

### `ota`

An update's progress, from either kind of update: `state` is `downloading`, `restarting` or
`failed`. A `failed` carries `error`, why, for a program to act on, and `detail`, for a
person. After `restarting` the port closes as the device restarts.

```json
{"evt":"ota","state":"failed","error":"verify_failed","detail":"sha256 does not match"}
```

| `error` | Why the update failed | Worth trying again? |
| --- | --- | --- |
| `download_failed` | Over the network: the image could not be fetched (the server, the connection, TLS, or the image cut short) | Yes |
| `cancelled` | Over USB: the port closed mid-update | Yes |
| `timeout` | Over USB: nothing was sent for 30 seconds | Yes |
| `write_failed` | The flash would not take the image, or (over USB) the first piece was not an image for this chip | Not as it is |
| `verify_failed` | The image's SHA-256 is not the one named, or it is not a valid image for this chip | Not with the same image |
| `bad_arg` | Over USB: a piece at the wrong offset or past `size`, or `ota_end` before all of it | Yes, from the start |

### Network status

What `net` answers and the `net` event carries, and what `info` holds under `net`. The
event is sent to USB whenever the network side changes state.

| Field | |
| --- | --- |
| `enabled` | Whether the network side is on |
| `wifi` | `off`, `no_network` (none configured), `connecting` or `connected` |
| `ssid` | The network joined or being joined, or `null` |
| `ip` | The address while connected, or `null` |
| `url` | The plugin's URL, or `null` |
| `token` | Whether a token is set (never the token) |
| `reader` | The scanner's id |
| `link` | The exchange with the plugin: `off` (not configured), `no_wifi`, `idle` (nothing tried yet), `ok`, `unreachable`, `refused` (401, 403 or 404: check the token and the reader id) or `error` (another status) |
| `last_status` | The last HTTP status: 0 before the first call, -1 when the server could not be reached |
| `poll_ms`, `wait_s` | The pacing, as set with `net` |
| `queued` | Messages waiting for the plugin to acknowledge |
| `dropped` | Messages lost since start: taps pushed out to make room, and anything too long to keep |

## Errors

| Code | Where | Meaning |
| --- | --- | --- |
| `bad_json` | `error` event | Not a JSON object with a `cmd` string |
| `line_too_long` | `error` event | Over 2047 bytes, or bytes lost on the way in |
| `unknown_cmd` | `rsp` | No such command, or not in this build |
| `bad_arg` | `rsp`, `ota` | A field missing or wrong; `detail` names it |
| `busy` | `rsp` | A job is waiting, an update is running, or the device could not take the command |
| `no_job` | `rsp` | `cancel` with nothing to cancel; an `ota_` piece for no update |
| `not_allowed` | `rsp` | Not from this link (see below) |
| `timeout` | `failed`, `ota` | No tag within `timeout_ms`; nothing sent of an update for 30 seconds |
| `cancelled` | `failed`, `ota` | `cancel`, `bootloader`, or the session that started it ended |
| `wrong_tag_type` | `failed` | Not an NTAG213, 215 or 216 |
| `multiple_tags` | `failed`, `tag` | More than one tag on the reader |
| `not_blank` | `failed` | The tag holds data and `overwrite` was not given |
| `auth_required` | `failed` | The tag is password protected and no password was given |
| `auth_failed` | `failed` | The tag refused the password |
| `locked` | `failed` | The tag is permanently locked |
| `too_large` | `failed` | The message does not fit this tag |
| `tag_removed` | `failed` | The tag left before the job finished |
| `write_failed` | `failed`, `rsp` to `ota_data`, `ota` | A write did not take |
| `verify_failed` | `failed`, `rsp` to `ota_end`, `ota` | What was read back is not what was written; an image that does not check out |
| `download_failed` | `ota` | An update's image could not be fetched over the network |
| `nfc_error` | `failed`, `rsp` to `program`/`wipe` | The NFC chip is not answering, or a tag exchange failed |

## Over the network

In a network build the plugin is a second link. Its commands and the device's answers and
events are the same objects, each given a `seq`, carried in the plugin's `POST sync/`
exchange: see docs/api.md in the plugin repository, and docs/network-transport-plan.md
here. The differences:

- From the network, `bootloader`, `debug`, `net`, `hid`, `ota_begin`, `ota_data` and
  `ota_end` are answered `not_allowed`: they need someone at the board. `ota` is allowed,
  for an image on the plugin's own server only.
- `rsp` goes back to the link that sent the command, as on USB. Events go as the table
  above says: the plugin gets taps, `tag_removed`, `ota` events and the events of its own
  jobs, but not `hello`, `log`, `net` or the events of jobs started over USB.
- The network link has no sessions: it is never closed, so its jobs end by their own
  timeout, not by a lapse in the network.

## Versioning

`hello` and `info` carry `proto`, which is 1. Hosts should ignore fields and events they
do not know, so that adding them does not need a new version; a change an older host would
misread does.

## Tools

`tools/nfcprog.py` speaks this protocol from the command line, `nfcprog.py raw` sends any
line, and `nfcprog.py monitor` prints what the device sends. The plugin's browser client
is `frontend/src/serial.ts` and `frontend/src/scanner.ts` in the plugin repository.
