# Open issues

Findings from a review of 2026-10-08 that were not fixed at the time. Each is verified
against the code as of commit `648eb3f`; line numbers drift, so the function names are the
anchor. Most are design-level gaps at the seams between this firmware, the InvenTree plugin
(`inventree-nfc-scanner-plugin`, which has its own `docs/open-issues.md`) and the outside
world, rather than bugs in any one function. The plugin's list carries the server side of the
shared items.

Severity: high = can lose work or strand a unit; medium = wrong behaviour in a realistic case;
low = rough edge.

## High

### 1. An answer over 8 KB jams the network link for good

- Where: `main/net_link.c` (`RESP_MAX`, the read loop in `do_call_once`),
  `components/net_sync/net_sync.c` (`net_sync_response`, the back-off on a body that does not
  parse); plugin side `sync.py` `handle_sync` step 3, which sends every pending command.
- Scenario: around 30 queued `program` commands (about 230 bytes each) for one reader make the
  `/sync` answer exceed `RESP_MAX`. The reader cuts it, the JSON does not parse, it backs off and
  calls again, and the answer is the same. Nothing is acknowledged in either direction; taps
  pile up and are dropped; even an `ota` cannot get through. The reader keeps calling, so the
  plugin never marks it offline. The simulator (`host_sim/main/sim_net.c`) uses a 16 KB buffer
  and treats an oversized answer as "unreachable", so `tools/test_sync.py` cannot catch this.
- Fix: cap commands per answer on the server (the reader takes two per call anyway: `cmd_batch_t`
  in `net_link.c`); on the reader, treat a cut answer as "ask again with a hint" rather than as a
  parse failure, or parse the commands that are whole. Make the simulator use the firmware's
  limits (`RESP_MAX` 8 KB, `BODY_MAX` 8 KB, 2 commands per answer) so the end-to-end test is
  faithful.

## Medium

### 2. A tag can type keystrokes into the host

- Where: `components/app_core/app_core.c` `lookup()` (the `hid_type` call), `printable()`;
  `main/Kconfig.projbuild` `APP_HID_DEFAULT_ON` defaults to y.
- Scenario: any tag's Text record is typed on the USB keyboard followed by Enter, filtered only
  for printable ASCII up to 127 characters. A planted tag holding a shell command runs it in
  whichever terminal has focus.
- Fix: type only text matching InvenTree's barcode pattern (`INV-SL<digits>`, or a Kconfig
  pattern), or default keyboard output off; say so in the README.

### 3. The 1200-baud download-mode trigger is live in production builds

- Where: `components/usb_dev/usb_cdc.c` (the line-coding callback), `main/main.c`
  `on_touch_1200`, `main/app_download_mode.c`.
- Scenario: any host program that opens the port at 1200 baud and closes it (an Arduino-style
  auto-reset, a serial prober) sends a unit in use into ROM download mode; the sticky flag keeps
  it there until a power cycle or esptool.
- Fix: gate the touch behind `CONFIG_APP_DEV_RECOVERY` (or its own option, off by default), and
  document it in the README alongside `idf_ext.py`.

### 4. A reader restart mid-job leaves the plugin's job stuck (shared with the plugin)

- Where: `components/net_sync/net_sync.c` acknowledges a command (`cmd_ack`) as soon as it is
  handed to the app task; `main/net_link.c` picks a new random `boot` each start and the RAM
  queue is lost; plugin side never compares `boot` and never enforces `Job.timeout_s`.
- Scenario: a job is `waiting`, the reader browns out or is power-cycled and calls in again
  within the offline window. The job stays `waiting` for ever and the machine stays busy; a
  later `cancel` is answered `no_job`.
- Fix: mostly server side (see the plugin's list). On the reader, consider answering a `cancel`
  for an unknown id with `ok: true` plus `detail: "no such job"`, so the server can end the job.

### 5. USB job ids collide with plugin job ids (shared with the plugin)

- Where: `components/app_core/app_core.c` broadcasts every job event to every link;
  `components/net_sync/net_sync.c` queues `waiting`, `writing`, `done` and `failed` for the
  server whatever link started the job; plugin side looks a job up by `id` within the machine.
- Scenario: `tools/nfcprog.py program` defaults to `--id 1`; its `done` reaches the plugin,
  which can match a plugin job with pk 1 (in particular one failed as `scanner_offline`, which
  the plugin re-opens on a late `done`) and link the UID to that job's location.
- Fix: carry the originating link in job events (or tag plugin jobs with a marker the reader
  echoes), and have `net_sync_queue` forward only events of jobs the server started.

### 6. Changing a tag's password is not tear-safe

- Where: `components/ntag21x/ntag21x.c` `protect()`: the new PWD page is written, then PACK,
  while AUTH0 is already active; `t->key` is updated only after the PWD write returns.
- Scenario: a tag pulled between the two writes keeps the new password with the old PACK and its
  protection. Repeating the job with `old_pwd` fails with `auth_failed`; only a host that knows
  to retry with the new password as the key can recover it. The README's tear-safety claim
  holds only for first-time protection.
- Fix: on a password change, authenticate and then write AUTH0 off, PWD, PACK, AUTH0 on (so an
  interrupted change leaves the tag open rather than half-changed), or document the recovery
  key. Also switch `t->key` to the new password before the write so a lost ACK can recover.

## Low

### 7. The trial-deadline restart ignores a running job

- Where: `main/ota.c` `confirm_deadline()` calls `esp_restart()` without checking
  `app_task_job_active()`; the updater's own restart does wait.
- Fix: wait for the job as the updater does, bounded.

### 8. A tag lifted just before a job starts fails the job at once

- Where: `main/app_task.c` offers the remembered tag to a new job while `s_tag_present` is
  still true; removal is declared only after `PRESENCE_MISSES` checks.
- Scenario: a tag lifted up to about 450 ms before a job arrives fails it with `tag_removed`
  instead of waiting for the next tag; over the network the job must be re-queued.
- Fix: before offering a remembered tag to a new job, check presence once.

### 9. Commands from one link affect another link's work

- Where: `components/app_core/app_core.c` `cmd_cancel` checks no owner; `log` is allowed from
  the network and resets every tag's level through `esp_log_level_set("*")`.
- Fix: refuse `cancel` for a job another link started (or require the id), and add `log` to
  `needs_hands()`.

### 10. Adding a network during a join tries another network first

- Where: `components/wifi_sta/wifi_sta.c` `wifi_sta_set_networks` disconnects; the disconnect is
  handled as a failed join, which `move_on()`s past the network just added.
- Fix: mark the disconnect as deliberate so the policy restarts at the preferred network.

### 11. Documentation that disagrees with the code

- `docs/network-transport-plan.md` says the station joins "the last one that worked"; in fact
  `preferred` is the network last given with `join` (`wifi_sta_last_joined()` is never used).
- The plan's "A unit nobody can reach" section still says images come from any https URL; from
  the network they must be on the plugin's own origin (`main/ota.c` `ota_start`).
- `README.md` and `tools/webserial.html` name InvenTree's `POST /api/barcode/link/` as the link
  endpoint; the plugin uses its own `api/location/<pk>/link/`, which moves a barcode that is
  already in use.
- A URI longer than 255 characters once expanded is dropped from taps
  (`components/ndef/ndef.c`, `NDEF_URI_MAX`), so a long base URL programs fine but taps report
  no `uri`.

### 12. The simulator's limits differ from the firmware's

| | Simulator (`host_sim/main/sim_net.c`) | Firmware (`main/net_link.c`) |
| --- | --- | --- |
| Commands accepted per answer | 4 | 2 |
| Answer buffer | 16 KB | 8 KB |
| Request body | 6 KB | 8 KB |
| Answer too big | unreachable | parsed cut short |

Align them, so that `tools/test_sync.py` exercises what the firmware does (see 1).

## Noted, not planned

- Updates have no image signing and no anti-rollback. A compromised plugin server or token can
  install any image, an older one included. A unit on a shared network out of USB reach should
  get secure boot before it is trusted; see "Credentials and trust" in the plan.
