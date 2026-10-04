# InvenTree NFC scanner

A desk NFC reader and programmer for an [InvenTree](https://inventree.org) inventory. NTAG215
tags sit in Gridfinity storage bins; each bin is an InvenTree stock location. The device

- **programs tags** from an InvenTree page over WebSerial. The page holds the InvenTree
  session and does every API call; the device needs no Wi-Fi, URL or token.
- **looks bins up** on its own: tap a tag and it types the tag's barcode (`INV-SL42`, Enter)
  as a USB keyboard, which InvenTree's scan field understands.

A tag carries one NDEF message with two records: a URI
(`https://<host>/web/stock/location/<pk>`), so a phone opens the bin's page, and a Text
record (`INV-SL<pk>`), InvenTree's short barcode. Tags are write-protected with the NTAG's
password, never with its permanent lock bits.

## Hardware

| Part | Connection |
| --- | --- |
| Waveshare ESP32-S3-Zero | 4 MB flash; native USB only, no UART bridge |
| PN532 | I2C at 0x24: SDA GPIO1, SCL GPIO2, 400 kHz. IRQ and RSTPD_N optional |
| RGB LED | onboard WS2812, GPIO21 |
| Piezo buzzer | optional, any GPIO |
| Logs | UART0 TX on GPIO43, 115200 baud |

Pins are set under *InvenTree NFC scanner* in `idf.py menuconfig`. Wiring RSTPD_N is worth
a GPIO: it is the only way to recover a PN532 that has stopped answering without unplugging
it. IRQ saves I2C traffic and changes nothing else.

## Build and flash

ESP-IDF v6.1.

```sh
idf.py build
idf.py flash
```

The firmware owns the board's only USB port, so esptool cannot reset it into the bootloader
the usual way. `idf_ext.py` handles that: before a flash it sends the running firmware the
`bootloader` command, waits for the ROM's USB-Serial-JTAG port (`303a:1001`) and flashes
through it; esptool's reset then returns the board to the app. Build first, as above: the
hook runs before the build, and a build that then fails leaves the board in download mode
until the next `idf.py flash` or `idf.py app-mode`.

- **A board not yet running this firmware:** hold BOOT while plugging it in, then
  `idf.py -p PORT flash`.
- `idf.py download-mode` and `idf.py app-mode` move a board between the two by hand.
- `idf.py monitor` shows nothing: logs go to UART0, or over the protocol with
  `tools/nfcprog.py log info`.

While `CONFIG_APP_DEV_RECOVERY` is on (the default during development), firmware that
crashes three times running, or that no host enumerates within 20 s of boot, puts itself in
download mode, so a bad build can be replaced without touching the board. **Turn it off for
a device in use**: powered from a charger, it would sit in download mode.

## Protocol

Newline-delimited JSON over the CDC port, one object a line. Every command gets one
immediate `rsp`; a job's progress follows as `evt` lines.

```
> {"cmd":"program","id":7,"ndef":"9101...","pwd":"A1B2C3D4"}
< {"rsp":"program","ok":true,"id":7}
< {"evt":"waiting","id":7,"timeout_ms":60000}
< {"evt":"writing","id":7,"uid":"04A1B2C3D4E5F6"}
< {"evt":"done","id":7,"uid":"04A1B2C3D4E5F6","type":"ntag215","protected":true}
```

| Command | Fields |
| --- | --- |
| `info` | |
| `program` | `id`, `ndef` (hex of the whole NDEF message), `overwrite`, `pwd` (8 hex), `pack` (4 hex), `old_pwd`, `timeout_ms` |
| `wipe` | `id`, `pwd`: empty the tag and remove its password |
| `cancel` | `id` (optional) |
| `hid` | `enabled`, `persist`: without `persist` it lasts until the port closes |
| `log` | `level`: `off`, `error`, `warn`, `info`, `debug` |
| `bootloader` | |

Events: `hello`, `waiting`, `writing`, `done`, `failed`, `tag`, `tag_removed`, `error`, `log`.

Errors: `bad_json`, `line_too_long`, `unknown_cmd`, `bad_arg`, `busy`, `no_job`, `timeout`,
`cancelled`, `wrong_tag_type`, `multiple_tags`, `not_blank`, `auth_required`, `auth_failed`,
`locked`, `too_large`, `tag_removed`, `write_failed`, `verify_failed`, `nfc_error`.

One job at a time. A tag already on the reader when a job starts is used at once. A tag
pulled away mid-write is left holding an empty, valid message. `not_blank` reports the text
and URI already on the tag so the page can ask before overwriting. After `done`, the page
links the UID to the location with `POST /api/barcode/link/`.

The mapping between lines and structs is `components/proto`; the behaviour is
`components/app_core`.

## Tools

| | |
| --- | --- |
| `tools/nfcprog.py` | The protocol from the command line: `info`, `program --host H --pk N`, `wipe`, `cancel`, `hid on\|off`, `log LEVEL`, `monitor`, `bootloader`. Needs pyserial. |
| `tools/webserial.html` | The same from a browser (Chrome or Edge), standing in for the InvenTree plugin page. |
| `tools/test_sim.py` | Runs `nfcprog.py` against the host simulator. |
| `tools/hid_check.py` | Checks keyboard output: turns it on for one session and reads back what a tap types. |
| `tools/tag_checks.py` | Guided checks with real tags: says which tag to present, sends the jobs, checks the answers. |

## Tests

Nothing below needs the board.

```sh
# Unit tests: NDEF, PN532 frames, NTAG operations, protocol, state machine
cd host_test && idf.py --preview set-target linux && idf.py build && ./build/host_test.elf

# The firmware's logic as a host program on a pty, driven by the real harness
cd host_sim && idf.py --preview set-target linux && idf.py build
python tools/test_sim.py
```

The NTAG tests run against a simulated tag (`host_test/components/sim_ntag`) that models
password protection, the one-time-programmable pages and a tag leaving the field. Among
other things they pull the tag away before every write of a job in turn and check that what
is left always reads as empty or complete.

What only real tags can confirm, and the set of tags to prepare for it, is in
[docs/test-tags.md](docs/test-tags.md).

## Layout

```
main/                 start-up, the task that owns the reader, download mode, recovery guard
components/app_core   the state machine: commands, tag events and time in; events out
components/proto      JSON lines <-> commands and events
components/ndef       NDEF parsing and the Type 2 TLV
components/ntag21x    NTAG213/215/216: read, tear-safe write, verify, password
components/pn532      frame codec and I2C driver
components/usb_dev    TinyUSB: CDC line link, HID keyboard, descriptors
components/feedback   LED and buzzer
host_test/, host_sim/ host builds of everything above the drivers
idf_ext.py            the flash hook
```

`app_core`, `proto`, `ndef` and `ntag21x` are plain C with no ESP-IDF in them, which is what
lets them run on the host, and what will let a network transport sit beside USB later.
