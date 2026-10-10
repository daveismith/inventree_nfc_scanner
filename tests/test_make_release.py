"""tools/make_release.py: what it refuses to package, and the manifest the plugin reads.

Run against a made-up project and build (version.txt, the two headers it reads, a build
directory with an app image), with esptool's merge replaced by a stand-in.
"""

import hashlib
import json
import subprocess
import sys

import pytest

import make_release

APP = b"\xe9" + b"app image" * 100


@pytest.fixture
def project(tmp_path, monkeypatch):
    """A project at version 1.2.3 with a production build; `project.run(*args)` packages it."""
    root = tmp_path / "project"
    (root / "components/app_core/include").mkdir(parents=True)
    (root / "main").mkdir()
    (root / "version.txt").write_text("1.2.3\n")
    (root / "components/app_core/include/app_types.h").write_text("#define APP_PROTO_VERSION 1\n")
    (root / "main/settings.h").write_text("#define SETTINGS_NET_VERSION 2\n")
    build = root / "build"
    (build / "bootloader").mkdir(parents=True)
    (build / "inventree_nfc_scanner.bin").write_bytes(APP)
    (build / "bootloader/bootloader.bin").write_bytes(b"boot")
    (build / "flash_args").write_text(
        "--flash_mode dio --flash_size 4MB\n0x0 bootloader/bootloader.bin\n0x20000 inventree_nfc_scanner.bin\n"
    )
    (build / "project_description.json").write_text(json.dumps({
        "project_version": "1.2.3", "target": "esp32s3", "git_revision": "v6.1",
        "app_bin": "inventree_nfc_scanner.bin", "config_file": str(build / "sdkconfig"),
    }))
    (build / "sdkconfig").write_text("CONFIG_APP_DEV_RECOVERY is not set\n")
    monkeypatch.setattr(make_release, "ROOT", str(root))

    real_run = subprocess.run

    def run(cmd, *args, **kwargs):
        if "merge-bin" in cmd:  # esptool's merge: an image that is all the parts, in order
            out = cmd[cmd.index("--output") + 1]
            with open(out, "wb") as f:
                f.write(b"merged" + APP)
            return subprocess.CompletedProcess(cmd, 0)
        return real_run(cmd, *args, **kwargs)

    monkeypatch.setattr(make_release.subprocess, "run", run)

    class Project:
        path = root
        out = tmp_path / "dist"

        def run(self, *args):
            monkeypatch.setattr(sys, "argv", ["make_release.py", "--build", str(build), "--out", str(self.out),
                                              "--git-sha", "a" * 40, *args])
            make_release.main()
            return json.loads((self.out / "manifest.json").read_text())

    return Project()


def test_the_manifest(project):
    m = project.run("--tag", "v1.2.3")
    assert m["name"] == "inventree_nfc_scanner" and m["version"] == "1.2.3"
    assert m["target"] == "esp32s3" and m["idf"] == "v6.1"
    assert m["proto"] == 1 and m["settings_version"] == 2
    assert m["min_plugin"] == make_release.MIN_PLUGIN and m["git_sha"] == "a" * 40
    assert m["dev"] is False
    assert m["app"] == {"file": "inventree_nfc_scanner-1.2.3.bin", "size": len(APP),
                        "sha256": hashlib.sha256(APP).hexdigest(), "offset": 0x20000}
    assert m["merged"]["file"] == "inventree_nfc_scanner-1.2.3-merged.bin" and m["merged"]["offset"] == 0
    assert (project.out / m["app"]["file"]).read_bytes() == APP


def test_a_tag_must_match_version_txt(project):
    with pytest.raises(SystemExit, match="does not match version.txt"):
        project.run("--tag", "v1.2.4")


def test_a_build_of_another_version_is_refused(project):
    (project.path / "version.txt").write_text("1.3.0\n")
    with pytest.raises(SystemExit, match="rebuild"):
        project.run()


@pytest.mark.parametrize("option", ["CONFIG_APP_DEV_RECOVERY=y", "CONFIG_APP_NET_ALLOW_HTTP=y",
                                    "CONFIG_APP_USB_TOUCH_1200=y"])
def test_a_development_build_is_not_a_release(project, option):
    (project.path / "build/sdkconfig").write_text(option + "\n")
    with pytest.raises(SystemExit, match="development build"):
        project.run()
    m = project.run("--dev")
    assert m["dev"] is True, "with --dev it is packaged, marked as such"


def test_a_development_build_is_never_released(project):
    with pytest.raises(SystemExit, match="never released"):
        project.run("--dev", "--tag", "v1.2.3")


def test_version_txt_must_hold_a_version(project):
    (project.path / "version.txt").write_text("one point two\n")
    with pytest.raises(SystemExit, match="not a version"):
        project.run()
