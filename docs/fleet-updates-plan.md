# Fleet firmware updates: plan (draft 2, for review)

Updating one or more scanners from InvenTree, whether they are on the network or plugged
into a user's computer over USB. Network scanners update at once; USB scanners update the
next time a browser connects to them.

Draft 2 takes these decisions: the firmware repository is public, so no GitHub credential is
needed anywhere; the server hosts the images and serves them without authentication, since
the device validates what it receives; serial (in-app) OTA for USB scanners; whether a user
may defer a USB update is a policy the admin sets; managing updates is for admins
only; images are stored in media; releases are checked automatically and on demand; room is
left for an admin flashing a scanner through the ROM bootloader from the browser.

## Status (2026-10-08, branch `fleet-updates` in both repositories)

Built and verified, phases 1 to 5:

| Piece | Where | Verified |
| --- | --- | --- |
| `version.txt`, `tools/make_release.py`, tag workflow `release.yaml`; CI packages every push | firmware | Locally; the tag workflow has not run yet (no tag pushed) |
| `fw` in the sync body; `reader` in `hello` and `info` | firmware | Host tests, simulator, the board |
| `ota_begin` / `ota_data` / `ota_end`, `components/ota_stream`, `nfcprog.py update` | firmware | 85 host tests, 52 simulator checks; the board, 1.2 MB in 25 s |
| Registry, firmware store, GitHub fetch, deployments, admin and USB API, image endpoint | plugin | `dev/check_fleet.py` (54 checks); `dev/check.py` (46) still passes |
| Network route | both | The board: deployed, downloaded, restarted and confirmed |
| USB route | both | The board, through `dev/usb_update.py`, which follows the browser's sequence |
| Browser: update notice and flow (`scanner.ts`), admin fleet item (`Fleet.tsx`) | plugin | Builds and lints; served, admin-only as intended; **not yet clicked in a browser** |

Changes from the plan as written:

- The image endpoint needs a signed-in user or a token, not none: every client that fetches
  it has one (the scanner sends its token to its own server, the browser its session), and it
  keeps the endpoint off the open internet for free.
- Lost jobs now end (`scanner_restarted` when a scanner calls with a new boot, `no_result` two
  minutes past a job's timeout). Without it, a stale job blocked an update for ever; it was the
  plugin's open issue 1.
- The network route's "did not come back" timeout is 20 minutes, beyond the firmware's own
  15-minute trial, so a rollback is reported as one.
- The reader stops polling while an image arrives over USB, which roughly halves the time.
- An admin can forget a scanner (`DELETE api/fleet/scanners/<reader>/`).

Not done:

- Phase 6 extras: GitHub artifact attestations; the ROM-bootloader flasher in the browser
  (the merged image and its manifest entry are in place for it).
- Fetching from the real repository, which is private until it goes public; the fetch is
  checked against a stand-in for GitHub's API.
- The first tagged release. Pushing `v0.2.0` after merging would exercise the workflow.

## What exists today

The firmware has a network OTA path: an `ota` command with a URL and a required sha256; the
image is fetched with the scanner's token, which it sends only to the plugin's origin; a
remote `ota` is accepted only from that origin; the written slot is hashed and compared; the
restart waits for a running job; the bootloader rolls back an image that does not confirm.

What is missing for a fleet: the server never learns a scanner's firmware version (the sync
body carries `reader`, `boot`, `proto`, `ack`, `wait_s`, `msgs` and nothing else); USB
scanners are not known to the server at all; a USB-only scanner has no way to receive an
image; and there is no release pipeline (`PROJECT_VER` is set by hand in `CMakeLists.txt`,
CI runs only on push and pull request).

## Shape of the design

### Releases

A workflow on tags `v*` builds the production configuration and publishes three assets:

- `inventree_nfc_scanner-<ver>.bin`: the app image, what OTA installs;
- `manifest.json`: version, sha256 and size of the app image, protocol version, settings-blob
  version, minimum plugin version, target board, IDF version, git sha, and the same for the
  merged image;
- `inventree_nfc_scanner-<ver>-merged.bin`: bootloader, partition table, otadata and app at
  their offsets, for first-time flashing with esptool and for the future browser flasher.

The version moves from `PROJECT_VER` in `CMakeLists.txt` to `version.txt`, which IDF reads
natively, and CI refuses a tag that does not match it. Pre-releases are marked as such on
GitHub. The upload action is pinned by hash like the others.

### Fetching releases

The repository is public, so the plugin lists releases and downloads assets with no token.
GitHub allows 60 unauthenticated API calls an hour per address; a check uses two. An optional
token setting exists only to raise that limit.

- A scheduled task checks every N hours (setting, default 24) and stores any release it does
  not have. A "Check now" button on the fleet page does the same at once.
- Validation at fetch: GitHub's asset digest, the manifest's sha256 and the sha256 of the
  downloaded bytes must agree, or nothing is stored and the admins are notified.
- Pre-releases are stored only when the "include pre-releases" setting is on.
- Fetched releases are never deployed by themselves unless the "deploy new stable releases to
  every scanner" setting is on. It is off by default.
- A manual upload form takes an image and its manifest, validated the same way, for sites
  without internet access.

### Storage and serving

Images are stored as media files under `plugins/nfcscanner/firmware/` in a `Firmware` model,
so InvenTree's backups and shared volumes cover them. They are served by a plugin view,
`GET /plugin/nfcscanner/firmware/<version>/<file>`, with no authentication, with
`Content-Length` and an `ETag`. A view rather than the media URL because InvenTree's standard
Caddyfile protects `/media/` with `forward_auth`, which demands a logged-in session a scanner
cannot present. (Draft 1 said media was served without authentication. It is not.) The view
holds a server worker for the download, five to ten seconds at the scanner's pace, so the
number of downloads in flight is capped by a setting, default two.

Serving without authentication costs nothing new: the image is public on GitHub already. What
matters is that a scanner installs only the image the server meant. The sha256 travels over
the authenticated sync channel inside the `ota` command, the device hashes the written slot
and compares, and a swapped or truncated file fails before any restart. The scanner still
sends its token on the connection, as it does to every same-origin URL; that is harmless.

### A registry of every scanner

A `Scanner` table keyed by reader id (`nfc-<mac>`): kind (network or USB), firmware
version, protocol version, last boot, last seen, last user. Network scanners fill it from
every sync once the body carries `fw`. USB scanners fill it from a browser check-in on
connect, which needs the firmware to report `reader` at top level in `hello` and `info` on
every build, since WebSerial does not expose the USB serial number. The fleet page lists every
scanner with its version, the newest available, and any pending or running deployment. The
dashboard item gets a "Connect scanner" button so a USB scanner can check in, and update,
without opening a location.

### Deployments

An admin picks a firmware and one or more scanners, or all, and one `Deployment` row is
created per scanner: firmware, scanner, requested by, requested at, `required` flag, state,
error, deferrals, finished at. States: pending, sent, downloading, restarting, confirmed,
failed, rolled back, superseded. A new deployment supersedes a pending one for the same
scanner. These rows are the audit trail, and the requester gets an InvenTree notification when
one fails.

Refused outright: a firmware whose protocol version the plugin does not speak, or whose
minimum plugin version is newer than the plugin. Downgrades need an explicit confirmation,
with one hard refusal: a network scanner may not go below the settings-blob version it runs,
because older firmware would reset its network settings and drop off the network. No eFuse
anti-rollback.

### Network scanners: at once

When a deployment is pending and the scanner is idle, the sync handler queues the existing
command, `{"cmd":"ota","id":<deployment>,"url":"<plugin origin>/plugin/nfcscanner/firmware/…","sha256":"…"}`.
With long polling on, the held request is released immediately. The server then follows what
the scanner already sends: a `busy` answer is retried a minute later; the `ota` events move
the row through downloading and restarting; the next sync with a new `boot` and the target
`fw` confirms it; a new boot still reporting the old version means the bootloader rolled it
back; no sync within five minutes of restarting is a failure. Jobs are not queued for a
scanner with a deployment in flight.

### USB scanners: on the next connection

The firmware gains serial OTA, a second byte source feeding the same slot, digest, confirm and
rollback machinery as the HTTPS path:

- `ota_begin` with id, size and sha256; `ota_data` lines carrying base64 chunks, each one
  answered so the browser never outruns the CDC buffer; `ota_end`. A chunk of about 1.2 KB
  fits the 2048-byte line, so a 1.9 MB image is about 1600 lines, 15 to 40 seconds. An `ota`
  event reports progress every few percent. Refused with `busy` while a job runs. Accepted
  from USB only, and from the plugin origin like `ota`.
- In the browser: on connect the panel reads `hello`, posts a check-in with reader id, version
  and protocol, and gets back a pending deployment or none. When there is one it blocks the job
  buttons, shows "Updating scanner to v1.2.0, do not unplug" with progress, fetches the image
  from the same origin, streams it, waits for the port to drop, reclaims it through the ports
  the user already granted (no click needed), reads the new `hello`, and reports the outcome.
  A scanner that comes back with the old version is reported as rolled back.

Whether the user may put it off is the admin's policy, a plugin setting with two values:
`required` (the update runs before the panel can be used) and `deferrable` (a dialog offers
"Update now" or "Later"; "Later" leaves the deployment pending, records the deferral, and the
dialog returns on the next connection). A deployment's `required` flag overrides the setting
for that deployment, so a security fix can be forced while the policy stays lenient. The fleet
page shows deferral counts so an admin can see who keeps saying later. An optional "required
after" date on a deployment turns a deferrable update into a required one on that day.

The user at the keyboard only carries bytes; the decision was the admin's. Check-in and
report need the same permission as programming a tag, and answer only for the scanner in hand.

### Admin flashing through the ROM bootloader (later)

Not in this plan's phases, but allowed for: the release carries the merged image and a
manifest that names its offsets; the serial layer in the browser exposes the port lifecycle so
an esptool-js flow can send `bootloader`, let the admin pick the ROM port (it is a different
USB identity, so a click is unavoidable), write the merged image, reset and reconnect; the
fleet page has a place for the button. That flow bypasses rollback and resets both slots, so
it is for recovery and first flashing, not routine updates.

### Who may manage updates

Only an admin. An admin here is a user whose group has InvenTree's `admin` role with change
permission, and superusers, who hold every permission. InvenTree does not expose Django's
per-model permissions in its UI; it has roles (admin, part, stock, build, and so on), each
granted to groups as view, add, change or delete, and mapped to models by its rulesets. A
plugin's own models are in no ruleset, so the plugin checks the admin role directly, through
the machine models that sit in that ruleset: `machine.change_machineconfig`, which the same
people hold who configure the scanners as machines.

Admin-only, both in the API and in the UI:

- the fleet page itself, the scanner list with versions, deployments and their history;
- fetching releases, "Check now", uploading an image, deleting one;
- creating, cancelling and superseding deployments, the `required` flag and date;
- the plugin's update settings (plugin settings are already edited in the Admin Center).

What a non-admin can do is only what the update needs from the browser that happens to hold
the scanner: the check-in on connect, the "Update now" or "Later" dialog when the policy
allows deferral, and the outcome report. Those two endpoints accept a user with the tag
programming permission, since that user is the one with the scanner plugged in, and they
reveal only the deployment pending for that one scanner. A non-admin never sees other
scanners, versions or history. Every deployment records the admin who requested it.

The alternative reading, superusers only, is a one-line change in the permission check if
the admin role turns out to be given more widely than intended.

### Security summary

- No credential for GitHub anywhere. Integrity is one sha256 carried end to end: GitHub's
  asset digest, the manifest, the stored file, the command, the device's check of the slot.
- There is no signing. A compromised server can push any image; secure boot is the answer
  and stays out of scope, as the firmware's own comment says. GitHub artifact attestations
  (build provenance from the tag workflow) could later let the server verify that CI built the
  asset; cheap to add in CI, more work to verify in Python, so optional.
- The device's trust model is unchanged: remote updates only from the plugin origin, over TLS
  against the certificate bundle, plain HTTP in development builds only. A production server
  needs a certificate from a public CA, for sync and OTA alike; the README should say so.
- Serial OTA adds no exposure: anyone at the USB port can already reach download mode.
- Managing updates needs the admin role; the public image endpoint reveals only what GitHub
  already publishes.

## Work by component

| Firmware | Size |
| --- | --- |
| Tag workflow: build, manifest, merged image, release upload, version check, `version.txt` | M |
| `fw` in the sync body; `reader` at top level in `hello` and `info` on every build | S |
| Serial OTA commands, sharing everything below the byte source with the HTTPS path | L |
| Host simulator and host tests for serial OTA; `nfcprog ota --file` | M |

| Plugin | Size |
| --- | --- |
| Models: `Firmware`, `Scanner`, `Deployment`; admin pages; migration | M |
| Release fetch (scheduled and on demand), manual upload, validation, image endpoint, pruning | M |
| Sync: issue `ota` when idle, busy retry, outcome tracking, cap, timeouts, no jobs while updating | M |
| Browser: check-in, policy dialog, update flow with progress and reconnection, outcome report | L |
| Fleet page: versions, deploy to selected or all, required flag, history, deferrals, Check now | M |
| Settings: check interval, pre-releases, auto-deploy, policy, cap, optional GitHub token | S |
| Tests: fake plugin and sync checks, check.py cases, docs/api.md, READMEs | M |

## Phases

1. Release pipeline. Needs nothing else; gives a first tagged release to test against.
2. Fleet visibility: `fw` and `reader` from the firmware, the registry, the check-in, the
   fleet page listing versions. Small and useful on its own.
3. Firmware store: fetch, upload, validate, serve.
4. Network deployment, on the bench board, reusing the existing `ota` path.
5. Serial OTA in the firmware and simulator, then the browser flow with the deferral policy.
6. Polish: auto-deploy setting, downgrade guard, notifications, pruning, attestations if wanted.

## Defaults taken, unless you object

- Release check every 24 hours; pre-releases excluded; auto-deploy off.
- USB policy default `deferrable`; the "required after" date is included.
- Two downloads in flight; five minutes to come back after restarting.
- The last five releases are kept; older files are deleted, their records stay.
