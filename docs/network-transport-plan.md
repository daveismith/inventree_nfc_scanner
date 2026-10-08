# Network transport: plan

Status: proposed 2026-10-05. Built 2026-10-06 through N4 (see "Where it stands" at the end);
the plugin came first (see "Order of work").

## What this is for

Today the scanner is driven over USB by a page in a browser, which holds the InvenTree
session and makes every API call. A headless reader in an automated storage tower has no
browser and no USB host. Instead **the reader calls InvenTree**: it polls an InvenTree
plugin over HTTP for jobs, and posts what happens (taps, job results) back to it. The
plugin, on the server, does what the page does today, including linking a tag's UID to its
location.

This is an option in two senses:

- **Compiled in or not.** `CONFIG_APP_NET_ENABLE`. A build without it has no radio code and
  is the firmware as it was. `sdkconfig.defaults` turns it on, since the one board so far
  is both the desk unit and the network unit.
- **Switched on or not.** A build with it stays off the air until a network, a plugin URL
  and a token have been saved on the device, and can be turned off again, all over USB.

## In brief

| Question | Answer |
| --- | --- |
| Link | Wi-Fi station: a small module of this project's own that joins a saved network at boot and reconnects |
| Direction | The reader is an HTTP client of an InvenTree plugin. It listens on nothing |
| Getting jobs | Long polling, falling back to a poll every second against a server that does not hold the request |
| Reporting | Events are posted as they happen, not on the next poll |
| What is sent | The same `cmd`, `rsp` and `evt` objects as on USB, carried in JSON arrays |
| Credentials | The plugin's URL (`https://inventree.example/...` for this installation) and an InvenTree API token, set over USB and kept on the device |
| Setting it up | Over USB, with a new `net` command |
| Updates | A/B application slots and rollback, with ESP-IDF's `esp_https_ota`, before a unit is installed out of USB reach |

**Why the reader calls out, and nothing calls in.** InvenTree is served over https. A
reader that makes outbound https requests needs no certificate of its own, no open port, no
discovery, and no scheme of its own for deciding who may command it: InvenTree's token
already does that, and everything, tag passwords included, travels inside TLS. A plugin
endpoint is also the natural place for the logic that only the server can do.

## The exchange with the plugin

This is the contract the plugin implements (`inventree-nfc-scanner-plugin`, docs/api.md
there). There is one endpoint. The reader calls it whenever it has something to report, and otherwise at an
interval.

```
POST {plugin-url}/sync/          e.g. https://inventree.example/plugin/nfcscanner/sync/
Authorization: Token <inventree-api-token>
Content-Type: application/json

{
  "reader": "nfc-0123456789ab",     who is calling: fixed, from the MAC
  "fw": "0.2.0",                    the firmware it runs, for the plugin's fleet page
  "boot": 17,                       changes every time the reader restarts
  "proto": 1,
  "ack": 41,                        the highest command seq the reader has acted on
  "wait_s": 0,                      how long the server may hold this request; 0: answer now
  "msgs": [                         rsp and evt objects exactly as on USB, numbered
    {"seq": 12, "evt": "tag", "uid": "04A1B2C3D4E5F6", "text": "INV-SL4", "protected": true},
    {"seq": 13, "rsp": "program", "ok": true, "id": 7}
  ]
}

200
{
  "ack": 13,                        the highest msg seq the server has stored
  "cmds": [                         cmd objects exactly as on USB, numbered
    {"seq": 42, "cmd": "program", "id": 8, "ndef": "9101...", "pwd": "A1B2C3D4"}
  ],
  "poll_ms": 1000                   optional: how soon to call again when idle
}
```

- **Plain polling** is `wait_s: 0`: the server answers at once, and the reader calls again
  after `poll_ms`. A job reaches the reader within one interval.
- **Long polling** is the same request with `wait_s` above zero: the server may hold it
  until it has a command, or the time is up. A job reaches the reader at once. While a
  request is being held the reader uses a second one, with `wait_s: 0`, for anything it has
  to report, so a tap is never stuck behind a held poll.
- **The reader long-polls by default** (`wait_s` 25, under the 30 s most proxies allow) and
  **falls back by itself**: when nothing is happening it never calls more often than once a
  second, whatever the server does. A server that holds the request gets a job to the reader
  at once; one that answers straight away is simply polled every second, with no tight loop
  and no capability negotiation. A server may also send `poll_ms` to ask for a different
  idle interval. Long polling holds a server worker per reader for the duration; the plugin
  decides whether to hold.
- **Nothing is lost or done twice.** Each side numbers what it sends and acknowledges what
  it has received. The reader keeps events until they are acknowledged and sends them again
  if not; the server does the same with commands. The reader ignores a command number it
  has already acted on, and the server ignores an event number it has already stored.
  `boot` tells the server that the reader's numbering has started again.
- **When it goes wrong.** 401 or 403: the token is not accepted; the reader stops calling,
  says so in `net` status, and tries again slowly. Anything else that is not 200, or no
  answer: back off, from one second to thirty. Events wait in a bounded queue meanwhile; if
  it overflows the oldest taps are dropped and counted, and job results are kept.

A response to a report can carry commands too, so a plugin that reacts to a tap with a job
gets it to the reader without waiting for the next poll.

## What has to change first

`proto` does no I/O and `app_core` knows nothing about USB, which is what the first pass
set out to keep. But the code grew up with one host, and four assumptions need undoing
before a second one is added. None of this needs the radio, and all of it is testable on
the host.

1. **Answers go to whoever asked.** Every `rsp` is currently broadcast, because there is one
   listener. A command needs to carry where it came from, and its `rsp` return there.
   Events (`tag`, `waiting`, `done`, ...) go to every link.
2. **A session belongs to a link.** `hid` without `persist` lasts "until the port closes",
   and `hello` greets "the host". Both need to mean the link concerned.
3. **A job has an owner.** If the USB page that started a waiting job goes away, nobody is
   left to hear its outcome, and the next tag presented would be written unobserved. The job
   should be cancelled. A job from the plugin is different: the server is still its owner
   while the network is briefly down, so it waits out its own timeout.
4. **Some commands need the device in hand.** `bootloader` on a headless unit puts it in
   download mode with no USB host, which only a power cycle undoes. From the plugin it,
   `debug`, and changes to the network settings are refused with a new error, `not_allowed`.

Also in this first step, though it is not about links:

5. **The partition table moves once, now.** Updates over the network need two application
   slots, and resizing partitions later disturbs everything after them. Doing it at the
   start costs one USB flash and erases the stored settings (the HID default) once.

   ```
   nvs       0x009000   84K
   otadata   0x01E000    8K
   ota_0     0x020000  1.875M   the application (where factory is now)
   ota_1     0x200000  1.875M   empty until updates exist
   coredump  0x3E0000   64K
   ```

## Configuration

Compile time, under *InvenTree NFC scanner → Network* in menuconfig:

| Option | Default | |
| --- | --- | --- |
| `APP_NET_ENABLE` | n | Build the Wi-Fi station and the plugin link |
| `APP_NET_POLL_MS` | 1000 | Shortest interval between calls when idle: the plain-polling fallback |
| `APP_NET_WAIT_S` | 25 | How long the server may hold a request; 0 turns long polling off |
| `APP_NET_ALLOW_HTTP` | n | Accept a plugin URL that is not https (for a test server on the LAN) |

Run time, kept in NVS, set with the `net` command over USB:

```
> {"cmd":"net"}
< {"rsp":"net","ok":true,"enabled":true,"wifi":"connected","ssid":"workshop","ip":"192.168.1.57",
   "url":"https://inventree.example/plugin/nfcscanner","reader":"nfc-0123456789ab",
   "link":"ok","last_status":200,"poll_ms":1000,"wait_s":25,"queued":0,"dropped":0}
> {"cmd":"net","action":"join","ssid":"workshop","psk":"..."}
> {"cmd":"net","action":"forget","ssid":"workshop"}
> {"cmd":"net","action":"server","url":"https://inventree.example/plugin/nfcscanner","token":"..."}
> {"cmd":"net","action":"poll","poll_ms":500,"wait_s":20}
> {"cmd":"net","enabled":false}
< {"evt":"net","wifi":"connected","link":"ok"}
```

A passphrase and the token are accepted and never reported back. `info` gains a `net` field
with the same state. In a build without `APP_NET_ENABLE`, `net` is an unknown command.
`nfcprog.py` gains `net ...`, and `webserial.html` a network panel.

## Design

```
            USB CDC ──┐                           ┌── rsp ──▶ the link that asked
                      ├─▶ proto ─▶ app_core ──────┤
  plugin /sync ◀─────▶┘   (origin)                └── evt ──▶ every link
     (HTTP client)
```

- **Links.** `app_task` keeps a small table of links, each with a way to send and a session.
  USB is one; the plugin is another. `app_cmd_t` gains an `origin`, which `app_core` copies
  into the `rsp`.
- **`components/net_sync`: the exchange, with no network in it.** It holds the queue of
  unacknowledged messages, builds the request body, reads the response, decides what to
  hand to `app_core` and when to call again. The HTTP request itself is one function it is
  given. That keeps the part with the logic in it (numbering, retries, duplicates, back-off)
  in plain C, tested on the host like the rest.
- **The HTTP side** is `esp_http_client` over `esp-tls`, with the certificate bundle for
  public CAs, on a task of its own so that a slow server never delays the reader. One
  connection is kept open and reused; long polling adds a second.
- **Wi-Fi** is `components/wifi_sta`, written for this project so that it depends on nothing
  outside ESP-IDF: a station that keeps up to four networks in NVS, joins the last one that
  worked at boot, tries the others when that fails, reconnects with back-off when the link
  drops, and reports link changes through a hook that becomes the `net` event. It is a few
  hundred lines over `esp_wifi` and `esp_netif`; the station logic is kept apart from the
  radio calls so the host tests can cover the join-and-retry policy.
- **`tools/fake_plugin.py`** stands in for the plugin, as `nfcprog.py` stands in for the
  page: a small HTTP server that implements `/sync`, lets you queue a program job for a
  location from the command line, and prints what the reader reports. It is the test
  server for every phase below, and a reference for whoever writes the real plugin.

## Credentials and trust

- The reader stores an InvenTree API token, in NVS. The NVS partition is encrypted
  (`CONFIG_NVS_ENCRYPTION`, HMAC scheme): the XTS keys are derived in the HMAC peripheral
  from a key in eFuse block KEY0, which the firmware generates and burns itself on the
  first boot that finds the block empty. That burn is permanent and takes one of the six
  key blocks. What it protects against is a copy of the flash: read through download mode,
  the partition is ciphertext and the key is not readable from the fuses (the block is
  read-protected; only the peripheral sees it). What it does not protect against is code
  running on the chip, including a replacement firmware or a JTAG session over the same
  USB port, which can ask the peripheral to derive the same keys. Closing that would need
  the JTAG-disable fuses, and secure boot with flash encryption, which is a different
  project. The token should belong to a user that can do what the plugin needs and nothing
  more.
- The token and the plugin URL can be set only over USB.
- The server's certificate is checked against the built-in bundle of public CAs. A private
  CA would need its certificate built into the firmware.
- Commands from the plugin are trusted as far as the token is. `bootloader`, `debug` and
  `net` changes are still refused from there.

## A unit nobody can reach by USB

Two things about a headless unit differ from the desk unit, and both are about what happens
when something goes wrong:

- The development recovery guard must be **off**. With no USB host it would put the unit in
  download mode twenty seconds after every power-up.
- So bad firmware cannot be replaced through USB download mode. It is replaced by an update
  into the slot that is not running, started on trial: if the new image does not come up
  and confirm itself, the next reset boots the old one. ESP-IDF provides this: `esp_https_ota`
  fetches an image from a URL straight into the other slot, checking it as it goes, and
  with `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` the new image boots on trial until it calls
  `esp_ota_mark_app_valid_cancel_rollback()`. Here the plugin sends
  `{"cmd":"ota","url":"...","sha256":"..."}`, the reader fetches the image over https with
  the same certificate bundle, and the new firmware confirms itself once the reader chip has
  started and a `/sync` has succeeded.

This is phase N4. The link works without it; a unit should not be installed out of reach
without it. Images are served from wherever an https URL can reach; the token goes along
only when the URL is on the plugin's own server, so the plugin can serve uploaded firmware
to the reader's token without the image being public.

## Phases

Each ends in something testable; the first two need no radio.

| # | Scope | Needs | Exit test |
| --- | --- | --- | --- |
| N0 | Links and origins, sessions per link, job ownership, `not_allowed`, the new partition table | board, USB | Host tests for all of it. On the board: one USB reflash, then the existing no-tag checks and `tools/tag_checks.py` pass unchanged; closing the port cancels a waiting job |
| N1 | `net_sync`, `fake_plugin.py`, and `host_sim` talking to it over plain HTTP | nothing | A job queued on the fake plugin is written to the simulated tag and its `done` arrives once. With responses dropped at random: nothing lost, nothing done twice. Server stopped and restarted: events queued, then delivered. Reader restarted: the server notices `boot` |
| N2 | `wifi_sta`, the HTTP client and the `net` command on the board, against `fake_plugin.py` on the LAN | board, Wi-Fi | Join and set the URL over USB; program a tag from the fake plugin; a tap reaches it; USB and the plugin at once; router off and on again; image size and free heap measured; tag reads and writes still clean while requests are in flight; time from job queued to `waiting`, for plain and long polling |
| N3 | https, the token, refused commands, failure handling | board, Wi-Fi, a real InvenTree | The same checks against https. A wrong token: the reader stops, says why, and recovers when it is corrected. `bootloader` from the plugin: `not_allowed` |
| N4 | Updates with rollback; a "headless" configuration with the guard off | board, Wi-Fi | Update from a URL and come back on the new version; a deliberately broken image boots once and the old one returns by itself; on a charger with no host, the unit comes up and stays up |
| P | The InvenTree plugin: `/sync` as above, a job queue per reader, the barcode link on `done`, and its own page for programming tags | InvenTree | Comes first; see below |

## Order of work

The plugin came first: `inventree-nfc-scanner-plugin`, in the directory next to this one,
implements `/sync` as above and has been checked against InvenTree 1.4.3 (its
`dev/check.py`). `tools/sync_bridge.py` here is the first client of it, driving a USB
scanner through the exchange exactly as the firmware will, and is the reference for the
firmware's `net_sync`. The firmware's network phases can now start against the plugin's
local instance (`dev/` in the plugin repository), with `fake_plugin.py` still worth having
for host tests that need no InvenTree.

For this installation the reader will use `https://inventree.example/...`. The
server's certificate must come from a public CA so the reader's bundle accepts it.

## Risks

- **Size and memory.** The application is 350 KB now. Wi-Fi, lwIP, TLS and the HTTP client
  are expected to add roughly 700 to 900 KB; that is an estimate, to be measured in N2, and
  fits a 1.875 MB slot either way. Each TLS connection also takes tens of kilobytes of RAM
  while open. The board has 2 MB of PSRAM that is not in use and can be if needed.
- **Long polling on the server.** A held request occupies a worker for as long as it is
  held. Plain polling at one second is the safe default; whether to offer long polling is a
  question about the InvenTree deployment, not the firmware.
- **The Wi-Fi station is new code**, where a borrowed one would have been proven. It is
  small, its policy is host-tested, and the radio calls underneath it are ESP-IDF's own
  well-trodden path (`esp_wifi` station mode with the default event loop).
- **The reader chip and the radio share a board.** Tag exchanges are timed in milliseconds
  and Wi-Fi tasks run at higher priority. The driver already retries lost exchanges; N2
  checks that this is enough, with requests in flight during a write.
- **Power.** Wi-Fi transmit peaks are several hundred milliamps. A weak supply shows up as
  brownout resets; `info` already reports the reset reason.
- **A second host changes old behaviour.** Closing the USB page cancels a waiting job. That
  is the one deliberate change to the desk unit.

## To decide

Each has a default the plan above assumes.

Decided on 2026-10-05:

- The reader reaches InvenTree at a configured URL; `inventree.example` here.
- Long polling by default, falling back to a poll a second where the server does not hold.
- The token is in NVS, which is encrypted (HMAC scheme); see "Credentials and trust".
- The plugin is written first, by us.

- The project stays standalone: no esp-console-kit. Wi-Fi is a module of its own and
  updates use ESP-IDF's `esp_https_ota`.

Still assumed:

1. **Will an installed unit be reachable by USB?** Assumed not, hence N4.
2. **One firmware or two?** One source tree and a Kconfig switch; the default build has the
   radio in, and a desk-only unit can turn it off.

## Where it stands (2026-10-08)

Built and tested as far as a bench with no Wi-Fi credentials allows.

| Phase | State |
| --- | --- |
| N0 | Done. Commands carry an `origin`; `rsp` and `hello` go to the link that asked, events to every link; a link that closes cancels its own waiting job; `bootloader`, `debug`, `net` and `hid` from a remote link answer `not_allowed`. The partition table has two application slots and `otadata`; NVS is encrypted (HMAC scheme, eFuse KEY0). Host tests cover the links (`host_test/main/test_app_core.c`). |
| N1 | Done. `components/net_sync` is the exchange with no network in it, host-tested (`test_net_sync.c`). `tools/fake_plugin.py` stands in for the plugin; `host_sim` speaks `/sync` over a plain socket when `SIM_SYNC_URL` is set; `tools/test_sync.py` runs the phase's exit tests: a job once, nothing lost or doubled with answers dropped at random, a server that goes away and comes back, a reader that restarts, remote refusals, long polling against plain polling. 21 checks. |
| N2 | Built: `components/wifi_sta` (policy host-tested in `test_wifi_policy.c`), `main/net_link.c` with two HTTP tasks (one polls and may be held, one reports meanwhile), the `net` command and its settings in NVS, `nfcprog.py net ...`, a panel in `webserial.html`. On the board (2026-10-06): the new partition table is in, eFuse KEY0 is burnt with purpose HMAC_UP and read-protected, settings survive a reboot through the encrypted store, the station tries a network that does not exist and gives up when told to forget it, and the plugin URL and token are set. The simulator's link against the plugin's Docker instance runs a job from InvenTree's API to `done` with the barcode linked. What is left needs a network to join. |
| N3 | Built: https through the certificate bundle, `Authorization: Token`, 401/403/404 stop the link for a minute at a time until it is reconfigured, other failures back off up to 30 s. On the board: pending. |
| N4 | Done. `main/ota.c`, the `ota` command (from USB any allowed URL; from the network only the plugin's own origin), rollback on in the bootloader, the new image confirms itself once the reader chip (or 60 s) and a host (USB enumerated, or one `/sync` answered) have come up, and restarts after 15 minutes unconfirmed so the previous image returns; the `sha256` is required and checked before the image is used; the restart waits for a running job. On the board (2026-10-08): a 0.1.1 image served through the plugin's proxy was fetched, booted, confirmed, and survived a reset; a USB flash afterwards returned the board to the first slot. |

Decided on 2026-10-06: NVS encryption with the HMAC scheme (not flash encryption, which
would change the USB flashing workflow and close download mode in release mode); the
plugin serves firmware images to the reader's token.

What remains: the plugin side of OTA (an endpoint that holds uploaded firmware and serves
it to a scanner's token). The recovery guard is off in the default build and on only in
`sdkconfig.dev`, so a headless unit is the default build.

### Review fixes (2026-10-07)

A review of both projects found, and these were fixed: a deadlock when a command from the
plugin failed to parse (its error answer re-took the net link's lock); the updater matching
the plugin's origin by string prefix, so a lookalike host would have received the token; a
use after free in the updater's success path; the recovery guard on in the default build;
cJSON's 1000-level nesting limit against small task stacks; a tight loop when the plugin
answered without acknowledging; `hid` accepted from the network. Policy settled then: an
update asked for over the network must come from the plugin's own origin and name its
digest; the digest is always required; `hid` is refused from the network too; a new server URL drops the stored token unless a new
one comes with it; a stored `http://` URL leaves the link off in a build that forbids http.
