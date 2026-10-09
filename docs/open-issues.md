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

### 1. An answer over 8 KB jams the network link for good (mitigated for 1.0)

- Mitigated: the plugin sends at most two commands per answer from 1.0.0, which is this
  firmware's `min_plugin` (`tools/make_release.py`), so an answer stays far under 8 KB. The
  simulator now has the firmware's limits (two commands taken per answer, an 8 KB answer cut
  and parsed as the firmware does), and `tools/test_sync.py` drains a backlog of forty.
- Still open: against a server that sends more (an older plugin, or another server), a cut
  answer is still a parse failure and the link still stalls. Parsing the commands that are
  whole, or asking again with a smaller batch, would make the reader robust on its own.

### 2. A tag can type keystrokes into the host (fixed for 1.0)

- Fixed: a tap types its text only when it is an InvenTree barcode (`CONFIG_APP_HID_BARCODE_PREFIX`,
  `INV-` by default, two capital letters, one to ten digits); `CONFIG_APP_HID_TYPE_ANY` types
  any printable text instead. `app_core.c` `typeable()`, host test
  `test_lookup_types_only_barcodes`.

### 3. The 1200-baud download-mode trigger is live in production builds (fixed for 1.0)

- Fixed: `CONFIG_APP_USB_TOUCH_1200`, off by default and on in `sdkconfig.dev`; CI and
  `make_release.py` refuse a production build with it on. The flash hook still tries the
  `bootloader` command first; a production unit whose protocol does not answer needs BOOT.

### 4. A reader restart mid-job leaves the plugin's job stuck (shared with the plugin)

- Where: `components/net_sync/net_sync.c` acknowledges a command (`cmd_ack`) as soon as it is
  handed to the app task; `main/net_link.c` picks a new random `boot` each start and the RAM
  queue is lost; plugin side never compares `boot` and never enforces `Job.timeout_s`.
- Scenario: a job is `waiting`, the reader browns out or is power-cycled and calls in again
  within the offline window. The job stays `waiting` for ever and the machine stays busy; a
  later `cancel` is answered `no_job`.
- Fix: mostly server side (see the plugin's list). On the reader, consider answering a `cancel`
  for an unknown id with `ok: true` plus `detail: "no such job"`, so the server can end the job.
- Update (fleet-updates branch): the plugin now compares `boot` and fails the jobs a restarted
  reader had taken (`scanner_restarted`), and fails a job unreported past its timeout
  (`no_result`). The `cancel` answered `no_job` is still not acted on.

### 5. USB job ids collide with plugin job ids (fixed on this side for 1.0)

- Fixed: a job started over USB reports its progress to local links only
  (`APP_ORIGIN_LOCAL`, `app_core.c` `job_broadcast`); the server hears only of jobs it gave.
  The plugin also ignores events for jobs it did not send (its list, item 6).

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
- Deferred past 1.0: the plugin's README says to set the tag password once, before tags are
  programmed (the plugin cannot reach tags protected with an earlier password either; its list,
  item 7).

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

### 11. Documentation that disagrees with the code (fixed for 1.0)

- Fixed: the Wi-Fi preference and the update origin rule in `docs/network-transport-plan.md`;
  the link endpoint in `README.md` and `tools/webserial.html`; the 255-character URI limit on
  taps is now stated in the README.

### 12. The simulator's limits differ from the firmware's (fixed for 1.0)

- Fixed: `host_sim/main/sim_net.c` takes two commands per answer and cuts an answer at 8 KB,
  passing the cut body on to be parsed, as `main/net_link.c` does.

## Noted, not planned

- Updates have no image signing and no anti-rollback. A compromised plugin server or token can
  install any image, an older one included. A unit on a shared network out of USB reach should
  get secure boot before it is trusted; see "Credentials and trust" in the plan.

## Added after the review

### A release's `min_plugin` is set by hand and never checked

- Where: `MIN_PLUGIN` in `tools/make_release.py`; the plugin's `firmware.compatible` trusts it.
- Scenario: a firmware change needs plugin behaviour from a later release and the constant is
  not raised; older plugins are offered (and with automatic deployment sent) a firmware they
  cannot drive.
- Fix: `docs/compat-testing-plan.md`: a contract test of the host simulator against the plugin
  at `min_plugin`, gating the release workflow.
