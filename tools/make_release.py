#!/usr/bin/env python3
"""Package a production build as release assets: the app image, a merged image, a manifest.

    python tools/make_release.py [--build build] [--out dist] [--tag v0.2.0] [--git-sha SHA]

The app image is what an update installs (over the network or USB). The merged image holds the
bootloader, partition table, OTA data and app at their offsets, for flashing a blank board
with esptool from offset 0. The manifest describes both, and is what the InvenTree plugin
reads to decide whether, and to which scanners, a release may go:

    {"name": "inventree_nfc_scanner", "version": "0.2.0", "target": "esp32s3", "idf": "v6.1",
     "proto": 1, "settings_version": 1, "min_plugin": "0.1.0", "git_sha": "...",
     "app": {"file": "...", "size": 1234, "sha256": "...", "offset": 131072},
     "merged": {"file": "...", "size": 1234, "sha256": "...", "offset": 0}}

`settings_version` is the layout of the network settings this firmware writes: a firmware
with a lower one cannot read them, so a scanner on the network would lose its link. With
--tag, the tag must be "v" + the version in version.txt, or nothing is written.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The oldest plugin that can drive this firmware. Raise it when the firmware needs something
# a plugin release added.
MIN_PLUGIN = "0.1.0"

NAME = "inventree_nfc_scanner"


def define(path, name):
    """The integer a #define in one of our headers gives `name`."""
    with open(os.path.join(ROOT, path)) as f:
        m = re.search(rf"^#define\s+{name}\s+(\d+)", f.read(), re.M)
    if not m:
        sys.exit(f"{name} not found in {path}")
    return int(m.group(1))


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def flash_args(build):
    """(options, [(offset, file)]) from the build's flash_args."""
    with open(os.path.join(build, "flash_args")) as f:
        lines = [ln.strip() for ln in f if ln.strip()]
    options = lines[0].split()
    parts = [(int(off, 0), path) for off, path in (ln.split(None, 1) for ln in lines[1:])]
    return options, parts


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--build", default=os.path.join(ROOT, "build"))
    ap.add_argument("--out", default=os.path.join(ROOT, "dist"))
    ap.add_argument("--tag", help="the release tag, checked against version.txt")
    ap.add_argument("--git-sha", help="the commit built (default: git rev-parse HEAD)")
    args = ap.parse_args()

    with open(os.path.join(ROOT, "version.txt")) as f:
        version = f.read().strip()
    if not re.fullmatch(r"\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?", version):
        sys.exit(f"version.txt holds {version!r}, not a version like 1.2.3 or 1.2.3-rc.1")
    if args.tag is not None and args.tag != f"v{version}":
        sys.exit(f"tag {args.tag} does not match version.txt ({version}); expected v{version}")

    with open(os.path.join(args.build, "project_description.json")) as f:
        desc = json.load(f)
    if desc.get("project_version") != version:
        sys.exit(f"the build is of {desc.get('project_version')!r}, version.txt says {version}: rebuild")
    with open(os.path.join(args.build, "sdkconfig") if os.path.exists(os.path.join(args.build, "sdkconfig")) else desc["config_file"]) as f:
        config = f.read()
    for option in ("CONFIG_APP_DEV_RECOVERY=y", "CONFIG_APP_NET_ALLOW_HTTP=y"):
        if option in config:
            sys.exit(f"{option} is set: this is a development build, not a release")

    git_sha = args.git_sha
    if not git_sha:
        git_sha = subprocess.run(["git", "-C", ROOT, "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()

    options, parts = flash_args(args.build)
    app_offset = next(off for off, path in parts if path == desc["app_bin"])

    os.makedirs(args.out, exist_ok=True)
    app_file = f"{NAME}-{version}.bin"
    merged_file = f"{NAME}-{version}-merged.bin"
    shutil.copyfile(os.path.join(args.build, desc["app_bin"]), os.path.join(args.out, app_file))

    cmd = [sys.executable, "-m", "esptool", "--chip", desc["target"], "merge-bin",
           "--output", os.path.join(args.out, merged_file)] + options
    for off, path in parts:
        cmd += [hex(off), os.path.join(args.build, path)]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)

    def asset(file, offset):
        path = os.path.join(args.out, file)
        return {"file": file, "size": os.path.getsize(path), "sha256": sha256(path), "offset": offset}

    manifest = {
        "name": NAME,
        "version": version,
        "target": desc["target"],
        "idf": desc.get("git_revision", ""),
        "proto": define("components/app_core/include/app_types.h", "APP_PROTO_VERSION"),
        "settings_version": define("main/settings.h", "SETTINGS_NET_VERSION"),
        "min_plugin": MIN_PLUGIN,
        "git_sha": git_sha,
        "app": asset(app_file, app_offset),
        "merged": asset(merged_file, 0),
    }
    with open(os.path.join(args.out, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
