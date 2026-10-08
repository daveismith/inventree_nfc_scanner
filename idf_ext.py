"""idf.py extension that puts the board into ROM download mode before flashing.

The app owns the ESP32-S3's only internal USB PHY (TinyUSB, USB-OTG on GPIO19/20), which
leaves the USB-Serial-JTAG controller unreachable. esptool therefore has no hardware path to
EN/GPIO0 and cannot enter download mode by itself -- normally that means holding BOOT while
plugging the board in.

This closes the loop in software. Before any flash action, find the app's CDC port by VID:PID,
send it the protocol's `bootloader` command, wait for the ROM's USB-Serial-JTAG port to
appear, and point --port at it. esptool takes it from there, and its ESP32-S3 hard reset
clears RTC_CNTL_FORCE_DOWNLOAD_BOOT on the way out, so the board comes back up in the app.
The firmware half is main/app_download_mode.c.

If the protocol does not answer, a 1200 baud touch is tried: the firmware acts on that in the
USB callback, without going through the protocol at all.

Adapted from r2_domeplayer's idf_ext.py. Two differences. There is no UDP trigger: the
network link is a client of the InvenTree plugin and listens on nothing. And when no board is
found, the flash is stopped rather than left to esptool's port autodetection, which sends its
sync bytes to whatever serial devices it finds. The board that went into download mode is
recognised again by its USB serial number (its MAC), so another ESP32 on the bench is never
flashed by mistake; with two scanners connected, -p must say which.

The probe is skipped when an explicit -p was given, when ESPPORT is set in the environment
(idf.py folds that into --port), or when IDF_NO_AUTO_DOWNLOAD=1.

Build before flashing: this hook runs before the build does, so a build that then fails
leaves the board in download mode. That is recoverable -- `idf.py flash` finds it there and
carries on, and `idf.py app-mode` resets it back into the app -- but it is a board that is
not running until one of those is done.
"""

import os
import subprocess
import sys
import time

ESPRESSIF_VID = 0x303A

# CONFIG_TINYUSB_DESC_CUSTOM_PID in sdkconfig.defaults -- keep the two in sync.
APP_PID = 0x4E46

# Ports esptool can flash from. 0x1001 is the USB-Serial-JTAG controller, which is where we
# aim: once it has the PHY, esptool's own reset sequence drives EN/GPIO0 in hardware, so it
# can enter download mode by itself whether or not the ROM is already sitting there. 0x0009
# is the ROM's USB-OTG download mode, matched too in case the ROM holds on to OTG routing.
FLASHABLE_PIDS = {
    0x1001: 'USB-Serial-JTAG',
    0x0009: 'USB-OTG download mode',
}

# Action names taken from serial_ext.py's serial_actions dict. `erase_flash` is a separate,
# deprecated alias action rather than a synonym, so it needs listing too. `monitor` and
# `merge-bin` are deliberately absent.
FLASH_ACTIONS = {
    'flash',
    'app-flash',
    'bootloader-flash',
    'partition-table-flash',
    'encrypted-flash',
    'encrypted-app-flash',
    'erase-flash',
    'erase_flash',
    'erase-otadata',
    'read-otadata',
}

PORT_WAIT_SECONDS = 15.0
PORT_SETTLE_SECONDS = 0.5


def _note(message):
    sys.stderr.write(f'\033[0;33m{message}\033[0m\n')
    sys.stderr.flush()


def _ports_matching(pids):
    import serial.tools.list_ports

    return [p for p in serial.tools.list_ports.comports() if p.vid == ESPRESSIF_VID and p.pid in pids]


def _serial_of(port):
    """A port's USB serial number, normalised: the app reports the MAC as 12 hex digits, the ROM
    as colon-separated pairs."""
    return (port.serial_number or '').replace(':', '').upper()


def _find_flashable_port(serial=None):
    """A port esptool can flash from. With `serial`, only the board with that serial number;
    without, only if there is exactly one, since every ESP32 in download mode looks alike."""
    ports = _ports_matching(FLASHABLE_PIDS)
    if serial:
        ports = [p for p in ports if _serial_of(p) == serial]
    if len(ports) > 1:
        raise NoBoard(f'{len(ports)} boards are in download mode; pass -p to say which')
    return ports[0] if ports else None


def _find_app_port():
    ports = _ports_matching({APP_PID})
    if len(ports) > 1:
        raise NoBoard(f'{len(ports)} scanners are connected ({", ".join(p.device for p in ports)}); pass -p to say which')
    return ports[0] if ports else None


def _send_trigger(device):
    """Ask the running app to reboot into download mode, and report what it said."""
    import serial

    port = serial.Serial()
    port.port = device
    port.baudrate = 115200
    port.timeout = 0.1
    port.open()
    try:
        time.sleep(0.2)
        port.write(b'{"cmd":"bootloader"}\n')
        port.flush()
        # The board restarts about 150 ms after answering, so the port may vanish underneath
        # us mid-read. That is the success case, not an error.
        seen = b''
        deadline = time.time() + 2.0
        try:
            while time.time() < deadline:
                seen += port.read(256)
                if b'"rsp":"bootloader"' in seen:
                    break
        except Exception:
            return seen + b'<device detached>'
        return seen
    finally:
        try:
            port.close()
        except Exception:
            pass


def _touch_1200(device):
    """Open the port at 1200 baud and close it: the firmware's protocol-free trigger."""
    import serial

    try:
        port = serial.Serial(device, 1200)
        time.sleep(0.2)
        port.close()
    except Exception as e:  # noqa: BLE001 - the port vanishing here is the point
        _note(f'1200 baud touch: {e}')


def _wait_for_flashable_port(serial=None, timeout=PORT_WAIT_SECONDS):
    deadline = time.time() + timeout
    while time.time() < deadline:
        found = _find_flashable_port(serial)
        if found:
            # The /dev node can appear slightly before the interface is usable.
            time.sleep(PORT_SETTLE_SECONDS)
            return found
        time.sleep(0.25)
    return None


def _wait_for_app_port(timeout=PORT_WAIT_SECONDS):
    deadline = time.time() + timeout
    while time.time() < deadline:
        found = _find_app_port()
        if found:
            time.sleep(PORT_SETTLE_SECONDS)
            return found
        time.sleep(0.25)
    return None


class NoBoard(Exception):
    pass


def enter_download_mode():
    """Return a port esptool can flash from.

    A board that already presents one is returned as-is, which makes this idempotent and also
    recovers a board left in download mode by an interrupted flash. Raises NoBoard when there
    is neither an app nor a download-mode port to work with.
    """
    app_port = _find_app_port()
    # A board already in download mode (an interrupted flash) can only be told by its serial
    # number, which is this board's when no scanner is running alongside; with a scanner
    # present, the scanner is the board, and any lone ROM port is somebody else's.
    already = None if app_port else _find_flashable_port()
    if already:
        _note(f'A board is in download mode on {already.device} (serial {_serial_of(already) or "unknown"}); using it')
        return already

    if not app_port:
        raise NoBoard(
            f'No board at {ESPRESSIF_VID:04x}:{APP_PID:04x} and none in download mode. '
            'Plug it in; or, for a board not yet running this firmware, hold BOOT while '
            'plugging it in and pass its port with -p.'
        )

    serial = _serial_of(app_port)
    _note(f'Sending the bootloader command to {app_port.device} (serial {serial})')
    transcript = _send_trigger(app_port.device)

    found = _wait_for_flashable_port(serial)
    if not found:
        said = transcript[-120:] if transcript.strip() else b'nothing'
        _note(f'No download-mode port appeared within {PORT_WAIT_SECONDS:.0f}s (the device said: {said!r})')
        still_there = _find_app_port()
        if still_there:
            _note('Trying a 1200 baud touch instead')
            _touch_1200(still_there.device)
            found = _wait_for_flashable_port(serial)
    if not found:
        raise NoBoard(
            'The board did not enter download mode. If something else holds its port '
            '(a monitor, a browser tab), close it and try again; failing that, hold BOOT while '
            'plugging it in and pass its port with -p.'
        )

    if found.pid == 0x0009:
        _note(
            'Board came back on USB-OTG, not USB-Serial-JTAG: the ROM kept the OTG routing. '
            'Flashing will work, but esptool cannot reset out of it, so the board will stay '
            'in the bootloader afterwards.'
        )
    _note(f'Board is in download mode on {found.device} ({FLASHABLE_PIDS[found.pid]})')
    return found


def leave_download_mode():
    """Reset a board sitting in download mode back into the app, without flashing anything.

    esptool's ESP32-S3 hard reset clears the sticky FORCE_DOWNLOAD_BOOT bit, so any command
    that ends in one will do; reading the chip id is the cheapest.
    """
    app = _find_app_port()
    if app:
        _note('The board is already running the app')
        return app
    found = _find_flashable_port()
    if not found:
        raise NoBoard('No board in download mode')
    subprocess.run(
        [sys.executable, '-m', 'esptool', '--chip', 'esp32s3', '-p', found.device, '--after', 'hard-reset', 'chip-id'],
        check=True,
    )
    app = _wait_for_app_port()
    if not app:
        raise NoBoard('The board was reset but the app did not appear on USB')
    _note(f'App is running on {app.device}')
    return app


def action_extensions(base_actions, project_path):
    def global_callback(ctx, global_args, tasks):
        task_names = {task.name for task in tasks}
        if not (task_names & FLASH_ACTIONS):
            return
        if os.environ.get('IDF_NO_AUTO_DOWNLOAD') == '1':
            return
        if global_args.port:
            # An explicit --port (or ESPPORT) is the user's call; don't second-guess it.
            return

        if 'monitor' in task_names:
            _note(
                'Note: the port changes twice across a flash, so a chained `monitor` will '
                'point at a stale one. And this firmware logs to UART0, not to USB.'
            )

        try:
            found = enter_download_mode()
        except NoBoard as e:
            raise SystemExit(f'{e}\n(IDF_NO_AUTO_DOWNLOAD=1 skips this check.)')
        global_args.port = found.device

    def download_mode_action(action_name, ctx, args, **kwargs):
        try:
            found = enter_download_mode()
        except NoBoard as e:
            raise SystemExit(str(e))
        print(found.device)

    def app_mode_action(action_name, ctx, args, **kwargs):
        try:
            found = leave_download_mode()
        except NoBoard as e:
            raise SystemExit(str(e))
        print(found.device)

    return {
        # Required by merge_action_lists(); without it idf.py drops the extension with only
        # a warning. This is the extension-schema version, not our own.
        'version': '1',
        'global_action_callbacks': [global_callback],
        'actions': {
            'download-mode': {
                'callback': download_mode_action,
                'short_help': 'Reboot the board into ROM download mode and print its port',
                'help': (
                    'Send the bootloader command to the running app so the board reboots into '
                    'ROM download mode, then print the resulting serial port. Flash actions do '
                    'this automatically; this is for driving it by hand.'
                ),
            },
            'app-mode': {
                'callback': app_mode_action,
                'short_help': 'Reset a board in ROM download mode back into the app',
                'help': (
                    'Download mode is sticky: the board stays in it across resets until '
                    'esptool clears the flag. This does that without flashing, for a board '
                    'left there by an interrupted flash or by the recovery guard.'
                ),
            },
        },
    }
