# InvenTree NFC scanner

A desk NFC reader and programmer for an [InvenTree](https://inventree.org) inventory. NTAG215
tags sit in Gridfinity storage bins; each bin is an InvenTree stock location. The device

- **programs tags** from an InvenTree page over WebSerial. The page holds the InvenTree
  session and does every API call; on this route the device needs no Wi-Fi, URL or token.
- **looks bins up** on its own: tap a tag and it types the tag's barcode (`INV-SL42`, Enter)
  as a USB keyboard, which InvenTree's scan field understands. Only InvenTree barcodes are
  typed (the prefix, two capital letters, digits), so a planted tag cannot type a command into
  whatever has focus; `CONFIG_APP_HID_TYPE_ANY` types any printable text instead.

A tag carries one NDEF message with two records: a URI
(`https://<host>/web/stock/location/<pk>`), so a phone opens the bin's page, and a Text
record (`INV-SL<pk>`), InvenTree's short barcode. Tags are write-protected with the NTAG's
password, never with its permanent lock bits.

Known gaps are listed in [docs/open-issues.md](docs/open-issues.md).

## Hardware

| Part | Connection |
| --- | --- |
| Waveshare ESP32-S3-Zero | 4 MB flash; native USB only, no UART bridge |
| PN532 | I2C at 0x24: SDA GPIO1, SCL GPIO2, 400 kHz. IRQ and RSTPD_N optional |
| RGB LED | onboard WS2812, GPIO21 |
| Piezo buzzer | optional: a passive piezo between a GPIO and GND, or between two GPIOs (louder) |
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

That is the build for a unit in use. For development there is a second configuration file
with the recovery guard (the board sends itself to download mode when it crashes or no host
enumerates it, and the `debug` command) and plain `http://` plugin URLs, for the plugin's
local Docker instance:

```sh
idf.py -B build-dev -DSDKCONFIG=build-dev/sdkconfig -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.dev" build
idf.py -B build-dev flash
```

Keep the two apart, as above: their own build directory and their own `sdkconfig`. Without
`-DSDKCONFIG` both would share the `sdkconfig` in the repository root, and a plain
`idf.py build` after a development one would still carry the recovery guard and `http://`.
Both `-D` options are cached in the build directory, so they are needed once, on `build`.

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

### Updating a unit without the bootloader

A unit running this firmware also takes a new image over its own USB link, with no download
mode: `tools/nfcprog.py update build/inventree_nfc_scanner.bin` (about 25 s for 1.2 MB). The
image goes into the second application slot, its sha256 is checked, and the unit restarts into
it on trial, exactly as an update over the network. The InvenTree plugin uses the same commands
to update a USB scanner from the browser, and the `ota` command to update a network one.

### Releases

The version is the one line in `version.txt`. Pushing a tag `v<that version>` (or
`v<version>-rc.N` for a pre-release, after setting `version.txt` to match) runs
`.github/workflows/release.yaml`: it builds the production configuration and publishes a
GitHub release with the app image, a merged image for flashing a blank board from offset 0,
and `manifest.json`, which the plugin reads (see `tools/make_release.py`). A tag that does not
match `version.txt` fails the workflow. CI packages every push the same way without publishing.

```sh
echo 0.2.0 > version.txt && git commit -am "0.2.0" && git tag v0.2.0 && git push --follow-tags
```

Each release names the oldest InvenTree plugin that can drive it (`min_plugin` in the
manifest, `MIN_PLUGIN` in `tools/make_release.py`). Raise it whenever a firmware change relies
on something a plugin release added. The plugin holds a release that needs a newer plugin but
never deploys it. Nothing checks the number yet; `docs/compat-testing-plan.md` is the plan.

`tools/make_release.py --dev --build build-dev` packages a development build for uploading to
the plugin's local Docker instance (it allows plain http); never publish one.

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
| `net` | Network builds, USB only. No fields: report. `action`: `join` (`ssid`, `psk`), `forget` (`ssid`), `server` (`url`, `token`), `poll` (`poll_ms`, `wait_s`); `enabled` with or without an action. See docs/network-transport-plan.md |
| `ota` | Network builds. `url` of a firmware image and its `sha256`, both required; progress comes as `ota` events and the device restarts into the new image once no job is running. From the network the image must be on the plugin's own server |
| `ota_begin` | USB only, any build. `id`, `size` and `sha256` of an image about to be sent. `busy` mid-job or while another update runs |
| `ota_data` | `id`, `at` (where the piece goes: where the last one ended) and `data`, base64 of at most 768 bytes. Each is answered; a piece out of order ends the update |
| `ota_end` | `id`. The image is checked (size, sha256, a valid image for this chip) and the device restarts into it on trial; `verify_failed` otherwise. An update also ends when the port closes or after 30 s with nothing sent |

A command carries an origin: its `rsp` goes back to the link that sent it (the USB page, or
the plugin over the network), events go to every link. From the network, `bootloader`,
`debug`, `net`, `hid` and the three `ota_` commands answer `not_allowed`. Closing the USB port cancels a job it started.
A command that cannot be read is answered as a `rsp` under the name given, with
`unknown_cmd`; a line that is not a command object at all gets an `error` event.

`hello` (sent when the port opens) and `info` carry `fw`, the firmware version, and `reader`,
the scanner's id (`nfc-` and its MAC address), the same id it uses over the network. The
plugin knows a scanner by it, whether it is plugged in or on Wi-Fi.

Events: `hello`, `waiting`, `writing`, `done`, `failed`, `tag`, `tag_removed`, `error`, `log`,
`ota` (an update's progress: `downloading`, `restarting`, `failed` with `detail`), and in
network builds `net` (the link changed state).

Errors: `bad_json`, `line_too_long`, `unknown_cmd`, `bad_arg`, `busy`, `no_job`, `timeout`,
`cancelled`, `wrong_tag_type`, `multiple_tags`, `not_blank`, `auth_required`, `auth_failed`,
`locked`, `too_large`, `tag_removed`, `write_failed`, `verify_failed`, `nfc_error`,
`not_allowed` (not from this link).

One job at a time. A tag already on the reader when a job starts is used at once. A tag
pulled away mid-write is left holding an empty, valid message. `not_blank` reports the text
and URI already on the tag so the page can ask before overwriting. After `done`, the page
links the UID to the location through the plugin's `api/location/<pk>/link/`, which, unlike
InvenTree's own `/api/barcode/link/`, moves a barcode another item already holds (a tag
re-programmed for another bin). A tap reports a tag's URI only up to 255 characters once its
prefix is expanded; a longer base URL programs fine, but taps then report no `uri`.

The mapping between lines and structs is `components/proto`; the behaviour is
`components/app_core`.

## Tools

| | |
| --- | --- |
| `tools/nfcprog.py` | The protocol from the command line: `info`, `program --host H --pk N`, `wipe`, `cancel`, `hid on\|off`, `log LEVEL`, `monitor`, `bootloader`, `net ...`, `ota URL --file IMAGE`, `update IMAGE` (over USB), `ndef`, `raw`. Needs pyserial. Passphrases and tokens are prompted for unless given as options, and never echoed. With two scanners connected it insists on `--port`. |
| `tools/webserial.html` | The same from a browser (Chrome or Edge), standing in for the InvenTree plugin page. |
| `tools/test_sim.py` | Runs `nfcprog.py` against the host simulator. |
| `tools/fake_plugin.py` | A stand-in for the InvenTree plugin's `/sync`, with endpoints to queue commands, read what the reader reported, and drop answers at random. |
| `tools/test_sync.py` | The network link end to end: the host simulator against `fake_plugin.py` (a job once, a lossy link, a server that comes and goes, a reader restart, long polling). |
| `tools/sync_bridge.py` | Presents a USB scanner to the InvenTree plugin as a network scanner, speaking its `/sync` exchange; the reference client for the network firmware. |
| `tools/hid_check.py` | Checks keyboard output: turns it on for one session and reads back what a tap types. |
| `tools/make_release.py` | Packages a build as release assets with its manifest; the release workflow runs it. |
| `tools/tag_checks.py` | Guided checks with real tags: says which tag to present, sends the jobs, checks the answers. |

## Tests

Nothing below needs the board.

```sh
# Unit tests: NDEF, PN532 frames, NTAG operations, protocol, state machine, the plugin
# exchange, the Wi-Fi policy (each from the repository root)
(cd host_test && idf.py --preview set-target linux && idf.py build && ./build/host_test.elf)

# The firmware's logic as a host program on a pty, driven by the real harness; then the
# same over its network link against tools/fake_plugin.py
(cd host_sim && idf.py --preview set-target linux && idf.py build)
python tools/test_sim.py
python tools/test_sync.py
```

The NTAG tests run against a simulated tag (`host_test/components/sim_ntag`) that models
password protection, the one-time-programmable pages and a tag leaving the field. Among
other things they pull the tag away before every write of a job in turn and check that what
is left always reads as empty or complete.

What only real tags can confirm, and the set of tags to prepare for it, is in
[docs/test-tags.md](docs/test-tags.md).

## Layout

```
main/                 start-up, the task that owns the reader and its links, download mode,
                      recovery guard, the network link (net_link.c), and updates: trial and
                      USB (ota.c), over the network (ota_net.c)
components/app_core   the state machine: commands, tag events and time in; events out
components/proto      JSON lines <-> commands and events
components/net_sync   the exchange with the plugin, with no network in it
components/ota_stream an image sent over the serial link: order, size, timeout, link loss
components/wifi_sta   the Wi-Fi station; its join-and-retry policy is plain C
components/ndef       NDEF parsing and the Type 2 TLV
components/ntag21x    NTAG213/215/216: read, tear-safe write, verify, password
components/pn532      frame codec and I2C driver
components/usb_dev    TinyUSB: CDC line link, HID keyboard, descriptors
components/feedback   LED and buzzer
host_test/, host_sim/ host builds of everything above the drivers
idf_ext.py            the flash hook
```

`app_core`, `proto`, `ndef`, `ntag21x`, `net_sync`, `ota_stream` and `wifi_policy` are plain C with no
ESP-IDF in them, which is what lets them run on the host.

## Network link

With `CONFIG_APP_NET_ENABLE` (on in `sdkconfig.defaults`) the reader can also be driven by
the InvenTree plugin over Wi-Fi: it polls the plugin's `/sync/` for jobs and reports back,
and can be updated over the network. It stays off the air until told where to go, over USB:

```sh
python tools/nfcprog.py net join "workshop"        # asks for the passphrase
python tools/nfcprog.py net server https://inventree.example/plugin/nfcscanner   # asks for the token
python tools/nfcprog.py net            # what it is doing
```

The reader id it presents is `nfc-<mac>`, shown by `net` and `info`; it must match an NFC
Scanner machine in InvenTree, and the token must belong to that machine's user. Long
polling is asked for by default (`wait_s` 25) and falls back by itself to a poll a second
where the server does not hold. `CONFIG_APP_NET_ALLOW_HTTP` (on in `sdkconfig.dev` only,
for the plugin's local Docker instance) lets `net server` and `ota` take http:// URLs. A
build without it refuses them, and leaves a stored http:// URL unused.

The settings partition is encrypted, with keys derived from an eFuse HMAC key that the
firmware burns itself on the first boot that finds eFuse block KEY0 empty. That burn is
permanent. docs/network-transport-plan.md has the design, what the encryption does and
does not protect against, and the state of each phase.
