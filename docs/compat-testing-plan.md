# Checking a release's oldest supported plugin automatically: plan

Status: proposed (2026-10-08). Nothing here is built yet.

## The problem

Every release's `manifest.json` names `min_plugin`, the oldest InvenTree plugin that can drive
it. The plugin trusts it: a release whose `min_plugin` is newer than the plugin is held but
not deployed (`firmware.compatible` in the plugin). Today the number is a constant,
`MIN_PLUGIN` in `tools/make_release.py`, set by hand. If a firmware change starts relying on
plugin behaviour added in a later plugin release and nobody raises it, older plugins are
offered, and with automatic deployment sent, a firmware they cannot drive. Nothing would
notice until a scanner in the field misbehaved.

`min_plugin` is right when two things hold:

1. **The firmware works with the plugin at `min_plugin`.** This is the claim that matters, and
   it can be tested.
2. **It does not work with the release before** (the bound is tight). This is only advisory: a
   loose bound costs nothing but an upgrade the user did not need.

## The approach: a contract test of the firmware against a real plugin

The host simulator (`host_sim`) already runs the firmware's state machine, protocol, tag logic
and network link on Linux, and with `SIM_SYNC_URL`, `SIM_TOKEN` and `SIM_READER` it acts as a
network scanner against any `/sync`. `tools/test_sync.py` drives it against the fake plugin. The
same scenarios against a real InvenTree with the plugin at a chosen version are the contract
test.

### 1. One source for the number

- Move `MIN_PLUGIN` out of `tools/make_release.py` into a file, `compat.json`:
  `{"min_plugin": "0.2.0"}`. The release script reads it, and so does the contract test.
- A PR that raises it says why in the file's history, which is where a reviewer looks.

### 2. A contract test runner: `tools/contract_test.py`

Given an InvenTree URL, an admin token and the simulator binary, it:

- creates a scanner user, token and NFC Scanner machine for a reader id of its own, and a
  stock location (as the plugin's `dev/check.py` does);
- starts `host_sim` as that network scanner;
- runs the scenarios below, checking the results through the plugin's API;
- removes what it made.

The scenarios are the firmware's side of the contract, one per behaviour it relies on:

| Scenario | Proves |
| --- | --- |
| Sync, with `fw` and `reader` | The plugin accepts the body, records the scanner |
| A tap outside a job | `tag` events are stored as the last tap |
| Program job: queued, collected, tag presented, done | The job lifecycle, and the UID linked as the barcode |
| Cancel before and after collection | Both cancel paths |
| A restart mid-job | The plugin fails the lost job (`scanner_restarted`) |
| A firmware update over the network | The `ota` command's shape, `ota` events, the verdict after the new boot (needs the simulator to fetch over HTTP: see below) |

**The rule that keeps it honest:** a firmware change that needs something new from the plugin
adds a scenario for it in the same PR. The test then fails against any plugin older than the
one that added the behaviour, so `compat.json` must be raised for CI to pass.

The simulator needs one addition for the update scenario: the `ota` command fetching its image
over HTTP (it answers `unknown_cmd` today) and "restarting" under the image's version, as its
USB update already does.

### 3. Where it runs

**Firmware repository, `contract` CI job,** on every PR and on release tags:

- Start InvenTree (the version the plugin targets) as service containers: the server, Postgres
  and Redis, or SQLite to keep it light. Install the plugin with pip from its git tag
  `v<min_plugin>`, through `plugins.txt` as the plugin's dev setup does.
- Build `host_sim` from this commit and run `tools/contract_test.py` against it. **Failure
  blocks the PR, and in `release.yaml` the `publish` job depends on it, so a release whose
  `min_plugin` does not hold is never published.**
- Run it again against the plugin's `main` as an early warning: a plugin change about to break
  current firmware. Advisory, not blocking.
- Tightness, advisory: run against the plugin release just before `min_plugin`. If that passes
  too, the job summary says the bound could be lowered.

**Plugin repository, CI,** the reverse direction:

- The firmware's release workflow also publishes the simulator as an asset
  (`inventree_nfc_scanner-<version>-sim-linux-x86_64`), so the plugin needs no ESP-IDF.
- On every plugin PR: run the contract test (fetched from the firmware tag) against this
  plugin build, with the simulator of the latest firmware release and of the oldest release
  the plugin still claims to drive.
- That catches a plugin change that breaks firmware already in the field, which `min_plugin`
  cannot express.

### 4. What it costs

- InvenTree in CI: an image pull and migrations, about 3 to 5 minutes per job; caching the
  image and using SQLite keep it down.
- The plugin must be released (tagged) for its versions to be installable by version. Until
  the first plugin release, `min_plugin` 0.1.0 can be checked against the plugin's first
  commit on `main` that has `/sync`.

## Steps, in order

1. `compat.json`, read by `make_release.py`; document the rule in the README's release section.
2. `tools/contract_test.py` with the scenarios that need no simulator change, run locally
   against the plugin's Docker dev instance.
3. The `contract` CI job, blocking on `min_plugin` and advisory on `main`; `publish` depends on
   it.
4. Network `ota` in the simulator, and the update scenario.
5. The simulator as a release asset; the plugin's CI runs the contract test against the oldest
   and newest firmware it supports.
6. The advisory tightness run.
