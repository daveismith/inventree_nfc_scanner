# Open issues

What is known to be wrong or missing in this firmware, and not yet fixed. Started from a review
on 2026-10-08 and kept up to date since (last on 2026-10-08, with the fixes before 1.0). Line
numbers drift, so the function names are the anchor. Several items are shared with the
InvenTree plugin (`inventree-nfc-scanner-plugin`, its own `docs/open-issues.md`), which
carries the server side.

Severity: high = can lose work or strand a unit; medium = wrong behaviour in a realistic case;
low = rough edge. Fixed items are listed at the end, briefly, for the record.

## Medium

### 1. A cut answer stalls the link against a server that sends too much

- Where: `main/net_link.c` (`RESP_MAX` 8 KB, the read loop in `do_call_once`),
  `components/net_sync/net_sync.c` (`net_sync_response`, the back-off on a body that does not
  parse).
- Scenario: a server that sends more pending commands than fit in 8 KB (a plugin before 1.0,
  which sent them all, or another server) makes the reader cut the answer; it does not parse,
  the reader backs off and asks again, and gets the same answer. Nothing is acknowledged either
  way, until the server's queue shrinks.
- Mitigated: plugin 1.0 sends at most two commands per answer, and this firmware's `min_plugin`
  is 1.0.0, so a supported server never does this.
- Fix: parse the commands that are whole from a cut answer, or ask again with a hint for a
  smaller batch, so the reader copes on its own.

### 2. Changing a tag's password is not tear-safe (deferred past 1.0)


- Where: `components/ntag21x/ntag21x.c` `protect()`: the new PWD page is written, then PACK,
  while AUTH0 is already active; `t->key` is updated only after the PWD write returns.
- Scenario: a tag pulled between the two writes keeps the new password with the old PACK and its
  protection. Repeating the job with `old_pwd` fails with `auth_failed`; only a host that knows
  to retry with the new password as the key can recover it. The README's tear-safety claim
  holds only for first-time protection.
- Fix: on a password change, authenticate and then write AUTH0 off, PWD, PACK, AUTH0 on (so an
  interrupted change leaves the tag open rather than half-changed), or document the recovery
  key. Also switch `t->key` to the new password before the write so a lost ACK can recover.
- Deferred past 1.0: the plugin's README says to set the tag password once, before tags are
  programmed (the plugin cannot reach tags protected with an earlier password either: its list,
  "Tags protected with an earlier password").

## Low

### 3. A `cancel` for a job the reader no longer has is answered `no_job`

- Where: `components/app_core/app_core.c` `cmd_cancel`.
- Scenario: the reader restarted, or the job already ended; the plugin cancels; the reader
  answers `no_job`, which the plugin ignores, so the job ends only at the server's timeout.
- Fix: answer `ok: true` with `detail: "no such job"` for a cancel whose id is not running, so
  the server can end the job at once (the plugin's list, "A cancelled job the scanner no
  longer has").

### 4. The trial-deadline restart ignores a running job


- Where: `main/ota.c` `confirm_deadline()` calls `esp_restart()` without checking
  `app_task_job_active()`; the updater's own restart does wait.
- Scenario: it also restarts with the reader polling. A restart that cuts an I2C exchange
  short can leave the PN532 refusing its address until the unit is unplugged (the chip keeps
  its power through a restart, and the desk unit's module has no reset input on its header).
- Fix: wait for the job as the updater does, bounded, and rest the reader before restarting.

### 5. A tag lifted just before a job starts fails the job at once


- Where: `main/app_task.c` offers the remembered tag to a new job while `s_tag_present` is
  still true; removal is declared only after `PRESENCE_MISSES` checks.
- Scenario: a tag lifted up to about 450 ms before a job arrives fails it with `tag_removed`
  instead of waiting for the next tag; over the network the job must be re-queued.
- Fix: before offering a remembered tag to a new job, check presence once.

### 6. Commands from one link affect another link's work


- Where: `components/app_core/app_core.c` `cmd_cancel` checks no owner; `log` is allowed from
  the network and resets every tag's level through `esp_log_level_set("*")`.
- Fix: refuse `cancel` for a job another link started (or require the id), and add `log` to
  `needs_hands()`.

### 7. Adding a network during a join tries another network first


- Where: `components/wifi_sta/wifi_sta.c` `wifi_sta_set_networks` disconnects; the disconnect is
  handled as a failed join, which `move_on()`s past the network just added.
- Fix: mark the disconnect as deliberate so the policy restarts at the preferred network.

### 8. A just-updated unit refuses another update, saying only that it cannot write

- Where: `main/ota.c` `serial_begin` and `main/ota_net.c` (`esp_ota_begin` and
  `esp_https_ota_begin` return `ESP_ERR_OTA_ROLLBACK_INVALID_STATE` while the running image is
  still on trial).
- Scenario: an update is installed; within the minute before the new image confirms itself
  (or longer, if the reader chip is not answering), a second update is refused with "could not
  start writing the slot". It is the right refusal with the wrong words.
- Fix: name the reason ("this firmware is still on trial; try again once it has confirmed
  itself") and say how long that can take.

### 9. A release's `min_plugin` is set by hand and never checked


- Where: `MIN_PLUGIN` in `tools/make_release.py`; the plugin's `firmware.compatible` trusts it.
- Scenario: a firmware change needs plugin behaviour from a later release and the constant is
  not raised; older plugins are offered (and with automatic deployment sent) a firmware they
  cannot drive.
- Fix: `docs/compat-testing-plan.md`: a contract test of the host simulator against the plugin
  at `min_plugin`, gating the release workflow.

## Noted, not planned

- Updates have no image signing and no anti-rollback. A compromised plugin server or token can
  install any image, an older one included. A unit on a shared network out of USB reach should
  get secure boot before it is trusted; see "Credentials and trust" in the plan.

## Fixed, for the record

- **Closed, 2026-10-08: a stuck PN532 needs unplugging.** The PN532 keeps its power through
  a restart, and one that cuts an I2C exchange short can leave it refusing its address until
  power is removed. Updates over USB and the network rest the reader before they restart,
  which covers the restarts a unit in use sees. Still exposed: a flash through the bootloader
  (seen twice on the bench), the trial-deadline restart (item 4), crashes and brown-outs. No
  firmware reset is possible on the desk unit: the module's header has RSTO, a reset output,
  not RSTPD_N (README, "Hardware").

- **Fixed before 1.0:** keyboard output types only InvenTree barcodes; the 1200-baud touch is
  development-only; a USB job's events stay off the network link; the simulator has the
  firmware's network limits; documentation corrected (the Wi-Fi preference, the update
  origin, the link endpoint, the 255-character URI limit on taps).
- **Fixed with fleet updates:** jobs a restarted reader forgets are ended by the server
  (`scanner_restarted`, `no_result`); the USB update leaves the reader idle until its restart.
