# Connecting a Creality K1 to a Raspberry Pi

> This guide covers the current `pik1d`-based setup which replaces the
> earlier socat approach documented in the
> [original guide](https://rentry.co/k1-with-pi).

## Overview

`pik1d` is a small C daemon that bridges the K1 MCU serial ports to PTYs on the
Pi over USB CDC ACM links. Endpoint 0 is a dedicated control channel for
version checks, link liveness, and remote restart/shutdown commands. Endpoint 1
is the MCU serial mux. Endpoint 2 is a sibling `tcpbridge` process used by the
touchscreen Moonraker tunnel.

The TCP tunnel is intended for low-bandwidth Moonraker API traffic from the K1
touchscreen to the Pi. It should not be used for high-bandwidth traffic such as
webcam streams or file transfers.

## Prerequisites

- **Make sure your K1 mainboard has a populated micro-USB header. This is very
  risky to do unless you have a way to recover your K1 to stock.** See
  [Creality's recovery flashing instructions](https://github.com/CrealityOfficial/K1_Series_Annex/releases/tag/V1.0.0).
- You should already be using a probe supported by
  [Simple AF](https://pellcorp.github.io/creality-wiki/) because stock Klipper
  does not support the K1's multiple load cells. This guide assumes a
  Cartographer over USB, but most probes should work.
- You will need an SBC that supports USB OTG mode. This guide assumes a
  Raspberry Pi 4, but other devices are technically possible.

## Hardware

1. #### Power considerations
    The Pi may backfeed power to the K1 via the VCC+ line of the USB cable. **This is very
    risky and can cause all sorts of hard-to-debug issues.** The Pi and K1 must still share
    ground, so only the VCC+ line should be cut -- leave shielding and GND intact.

    Preventing backfeeding (pick one):
    - Kapton tape over the VCC+ pin of the USB cable plugged into the printer
    - Cable surgery to snip the VCC+ wire on the cable
    - A USB power blocker dongle (widely available)
    - [A printed jig](https://www.thingiverse.com/thing:3044586/files) to block the VCC+ pin
    - JST connectors wired to the K1 mainboard's USB header with the VCC+ wire removed

    Powering the Pi (its USB-C port is occupied by OTG, pick one):
    - A USB-C power/data splitter (still needs VCC+ cut on the K1 side)
    - A PoE adapter or HAT
    - Regulated voltage to the GPIO power pins if you know exactly what you are doing

    > **Note for Pi 5 users:** The Pi 5 has unique power requirements and software-side
    > checks. A USB-C power splitter can supply rated power with an appropriate supply.

2. #### Data cables
    - Connect the Pi's USB-C port to the K1's USB header, **having addressed the VCC+ issue
      above**. Shielded cables are strongly recommended.
        - Simple: USB-A male to USB-C male into the K1's front USB port
        - Neat: a JST to USB cable plugged directly into the K1 mainboard header
        - Combined: a USB-C power + data splitter for the Pi plus a JST to USB cable with
          the power wire snipped
    - Connect the Cartographer to the Pi over USB using the included cable
    - Recommended: unplug the camera cable from the K1, adapt it from JST to USB, and plug
      into the Pi
    - Optional: redirect the K1's front USB port to the Pi using the same JST adapter

    You may also need male/male or female/female JST adapter cables.

## Software

The following files are involved:

- `src/` -- C source for `pik1d`, `tcpbridge`, and the shared serial mux code
- `S99pik1.in` -- K1 init script template, rendered to `build/S99pik1`
- `setup_pik1.sh` -- Pi gadget setup script
- `pik1.service.in` -- Pi systemd service template

### Binaries

Pre-built binaries for K1 (`build/pik1d.mipsel`, `build/tcpbridge.mipsel`) and
Pi (`build/pik1d.aarch64`, `build/tcpbridge.aarch64`) are included in the repo
and are updated with each release. Normal installs use these included files.
On a development machine, you can optionally rebuild them from source with the
cross-compiler targets:

```bash
make toolchain   # one-time: downloads musl.cc cross-compilers into .toolchain/
make mipsel      # K1 binaries → build/pik1d.mipsel, build/tcpbridge.mipsel
make aarch64     # Pi binaries → build/pik1d.aarch64, build/tcpbridge.aarch64
make test        # native non-hardware unit and CLI smoke tests
```

### Raspberry Pi side

1. #### Install Simple AF for RPi
    Install [Simple AF for RPi](https://pellcorp.github.io/creality-wiki/rpi/).

2. #### Enable USB OTG mode
    Run the following script once to configure the Pi to act as a USB gadget. This
    puts the USB-C port into OTG/peripheral mode, which disables host mode on that
    port.

    ```bash
    #!/usr/bin/env bash
    set -euo pipefail

    BOOT=/boot/firmware   # Use /boot for pre-Bookworm images
    CFG="$BOOT/config.txt"
    CMD="$BOOT/cmdline.txt"

    # Enable dwc2 overlay (puts USB-C into OTG mode)
    grep -qxF 'dtoverlay=dwc2' "$CFG" || echo 'dtoverlay=dwc2' | tee -a "$CFG"

    # Add dwc2 to kernel cmdline (guard against running twice)
    grep -qF 'modules-load=dwc2' "$CMD" || sed -i.bak -E 's/$/ modules-load=dwc2/' "$CMD"

    # Autoload libcomposite at boot
    grep -qxF 'libcomposite' /etc/modules || echo 'libcomposite' | tee -a /etc/modules

    echo "Done. Reboot for changes to take effect."
    ```

    Reboot the Pi after running this.

    > **If the gadget fails to bind after reboot**, try changing `dtoverlay=dwc2`
    > to `dtoverlay=dwc2,dr_mode=peripheral` in `/boot/firmware/config.txt`. Some
    > Pi configurations require the mode to be set explicitly.

3. #### Install pik1
    From the repo directory on the Pi, using the included pre-built binaries:

    ```bash
    make install-pi
    ```

    This copies the binaries and setup script to `/opt/pik1/`, installs and enables
    `pik1.service` and the peer reboot/poweroff helper units, and runs
    `systemctl daemon-reload`. Pass `SUDO=` if running as root, or
    `PI_DIR=/your/path` to override the install prefix.

    The service runs `setup_pik1.sh` as root first (needed for configfs access),
    then starts `pik1d` as UID 1000 so the PTY devices it creates
    are accessible to Klipper.

    If there are any weird permissions errors then make sure UID 1000 (your klipper user,
    typically `pi` or similar) is in the `dialout` group so it can open `/dev/ttyGS0`
    through `/dev/ttyGS2`:

    ```bash
    sudo usermod -aG dialout $(id -un 1000)
    ```

4. #### Configure printer.cfg
    Add or update the MCU serial paths in your `printer.cfg`:

    ```ini
    [mcu]
    serial: /tmp/klipper_mcu
    restart_method: command

    [mcu nozzle_mcu]
    serial: /tmp/klipper_toolhead
    restart_method: command
    ```

    > `restart_method: command` is required. Hardware reset via DTR/RTS does not work over the serialmux tunnel as no hardware control lines are available.

### K1 side

1. #### Install Simple AF
    Install [Simple AF](https://pellcorp.github.io/creality-wiki/) on the K1.

2. #### Install pik1
    From a slim repo clone on the K1, such as `/root/pik1`:

    ```bash
    make install-k1
    ```

    This copies `build/pik1d.mipsel` and `build/tcpbridge.mipsel` to
    `/usr/data/pik1/`, renders and installs `/etc/init.d/S99pik1`, and
    disables the services below by renaming them with a `_` prefix so the init
    system skips them. To use a different install directory, pass the same
    `K1_DIR` value at install and uninstall time:

    ```bash
    make install-k1 K1_DIR=/your/install/path
    make uninstall-k1 K1_DIR=/your/install/path
    ```

    | Service | Reason |
    |---|---|
    | `S55klipper_service` | Must not run — K1 is now bridge-only |
    | `S56moonraker_service` | Must not run — no local Klipper |
    | `S55klipper_mcu` | Host MCU software, not needed |
    | `S50nginx_service` | Proxied Moonraker, no longer relevant |
    | `S50unslung` | Unrelated to printing |
    | `S50webcam` | Camera can be moved to the Pi |
    | `S99guppyscreen` | See optional TCP tunnel section below |

    If transferring via scp rather than running from a repo clone on the K1,
    render the init script locally with the same install path you will use on
    the printer:

    ```sh
    K1_DIR=/usr/data/pik1
    make render-k1-init K1_DIR="$K1_DIR"
    ssh root@<k1-ip> "mkdir -p $K1_DIR"
    scp build/pik1d.mipsel root@<k1-ip>:$K1_DIR/pik1d
    scp build/tcpbridge.mipsel root@<k1-ip>:$K1_DIR/tcpbridge
    scp build/S99pik1 root@<k1-ip>:/etc/init.d/S99pik1
    ssh root@<k1-ip> "chmod +x $K1_DIR/pik1d $K1_DIR/tcpbridge /etc/init.d/S99pik1"
    ```

    Then disable the K1-side services that should not start in bridge mode:

    ```sh
    ssh root@<k1-ip> '
    for svc in S50nginx_service S50unslung S50webcam \
        S55klipper_mcu S55klipper_service S56moonraker_service S99guppyscreen; do
        if [ -f /etc/init.d/$svc ]; then
            mv /etc/init.d/$svc /etc/init.d/_$svc
        fi
    done
    '
    ```

3. #### Reboot both devices
    Reboot the K1 so the renamed init scripts take effect. Reboot the Pi as well
    so the USB gadget and `pik1.service` start from a clean boot sequence.

    On the K1:

    ```sh
    reboot
    ```

    On the Pi:

    ```bash
    sudo reboot
    ```
    Upon start, the K1 daemon logs to `/tmp/pik1.log`.

## K1 touchscreen TCP tunnel

The TCP tunnel forwards the Simple AF K1 touchscreen's (guppyscreen) Moonraker
requests to the Pi over the dedicated third USB CDC ACM link. This is strongly recommended over the
alternative of pointing guppyscreen at the Pi's WiFi IP address -- WiFi is
unreliable enough that you will eventually lose display functionality mid-print.
The tunnel runs over the same wired USB connection as the control and MCU
bridges and stays up as long as the physical connection does.

The tunnel is low-bandwidth and intended for Moonraker API traffic only
(temperatures, print status, controls). Do not route webcam streams or file
transfers through it.

The included K1 init script and Pi systemd service enable this tunnel by default.
The K1 side uses `listen:` because guppyscreen connects there; the Pi side uses
`forward:` because it connects onward to Moonraker. guppyscreen requires no
configuration changes -- it continues talking to `localhost:7125` as normal and
the tunnel forwards those connections to the Pi transparently.

**K1 init script** -- `/etc/init.d/S99pik1` starts `pik1d` with physical MCU UARTs
and a local TCP listener:

```sh
DAEMON_ARGS="--usb $USB_ID \
    mcu:0:$CH0_DEV:$CH0_BAUD \
    mcu:1:$CH1_DEV:$CH1_BAUD \
    listen:$TCP_ADDR:$TCP_PORT"
```

Also re-enable guppyscreen if you disabled it:
```sh
mv /etc/init.d/_S99guppyscreen /etc/init.d/S99guppyscreen
```

`TCP_ADDR` is `127.0.0.1` by default so only local touchscreen requests are
accepted on the K1. Set it to `0.0.0.0` only if you deliberately want the
forwarded listener exposed on the K1 network interface. The TCP tunnel is not
authenticated; wildcard binds expose raw forwarded Moonraker traffic to any
client that can reach that port.

**Pi systemd service** -- `/etc/systemd/system/pik1.service` starts `pik1d` with
PTY destinations and a TCP forwarder:

```ini
ExecStart=/opt/pik1/pik1d --usb 1d6b:0104 \
    pty:0:/tmp/klipper_mcu \
    pty:1:/tmp/klipper_toolhead \
    forward:127.0.0.1:7125
```

Then reload and restart:

```bash
sudo systemctl daemon-reload
sudo systemctl restart pik1
```

No changes are needed to guppyscreen's configuration -- it continues
talking to `127.0.0.1:7125` as if Moonraker were local.

## Post-install verification

1. #### Pi service
    ```bash
    sudo systemctl status pik1
    journalctl -u pik1 -f
    ```
    After rebooting both devices, it should show `active (running)`. Journal
    lines include systemd timestamps and process
    metadata, and `pik1d`/`tcpbridge` messages may appear in a slightly different
    order. The message text should look like:
    ```
    setup_pik1: loading libcomposite
    setup_pik1: creating gadget at /sys/kernel/config/usb_gadget/pik1
    setup_pik1: binding gadget to UDC: fe980000.usb
    setup_pik1: gadget setup complete
    [pik1] uart=pty release=0.3.0 protocol=3 channels=2 tcp=forward:127.0.0.1:7125 control=usb[0] serial=usb[1] tunnel=usb[2]
    [ctrl] link opened: /dev/ttyGS0
    [ctrl] link up: release=0.3.0 protocol=3 features=0x00000000
    [pik1] control session started: control=/dev/ttyGS0 serial=/dev/ttyGS1 tunnel=/dev/ttyGS2
    [pik1] data session started: serial=/dev/ttyGS1 tunnel=/dev/ttyGS2
    [pik1] child: spawned /opt/pik1/tcpbridge pid=...
    [mux] link opened: /dev/ttyGS1
    [tcp] tcpbridge /dev/ttyGS2 forward 127.0.0.1:7125
    [tcp] link opened: /dev/ttyGS2
    [mux] link up
    [tcp] link up
    [mux] ch0 PTY /tmp/klipper_mcu -> /dev/pts/2
    [mux] ch1 PTY /tmp/klipper_toolhead -> /dev/pts/3
    ```

2. #### K1 log
    ```bash
    cat /tmp/pik1.log
    ```
    A normal startup looks like. Because `pik1d` and `tcpbridge` write to the
    same log, adjacent lines may occasionally interleave:
    ```
    2026-05-25 19:41:50 [pik1] uart=mcu release=0.3.0 protocol=3 channels=2 tcp=listen:127.0.0.1:7125 control=usb[0] serial=usb[1] tunnel=usb[2]
    2026-05-25 19:41:50 [ctrl] link opened: /dev/ttyACM0
    2026-05-25 19:41:50 [ctrl] link up: release=0.3.0 protocol=3 features=0x00000000
    2026-05-25 19:41:50 [pik1] control session started: control=/dev/ttyACM0 serial=/dev/ttyACM1 tunnel=/dev/ttyACM2
    2026-05-25 19:41:50 [pik1] data session started: serial=/dev/ttyACM1 tunnel=/dev/ttyACM2
    2026-05-25 19:41:50 [pik1] child: spawned /usr/data/pik1/tcpbridge pid=...
    2026-05-25 19:41:50 [mux] link opened: /dev/ttyACM1
    2026-05-25 19:41:50 [tcp] tcpbridge /dev/ttyACM2 listen 127.0.0.1:7125
    2026-05-25 19:41:50 [tcp] listening on 127.0.0.1:7125
    2026-05-25 19:41:50 [tcp] link opened: /dev/ttyACM2
    2026-05-25 19:41:50 [mux] link up
    2026-05-25 19:41:50 [tcp] link up
    2026-05-25 19:41:50 [mux] ch0 MCU active
    2026-05-25 19:41:50 [mux] ch1 MCU active
    ```
    If you changed `TCP_ADDR` in `/etc/init.d/S99pik1`, the logged TCP address
    will match that configured value.

3. #### dmesg (Pi)
    ```
    [    7.614279] dwc2 fe980000.usb: bound driver configfs-gadget.pik1
    [    7.846025] dwc2 fe980000.usb: new device is high-speed
    [    7.900000] dwc2 fe980000.usb: new address ...
    ```

4. #### Klipper behaviour
    Klipper should connect to both MCUs within about 15 seconds of the K1 booting --
    this is the GD32 bootloader dwell time and is normal. `FIRMWARE_RESTART` also takes
    approximately 15 seconds for the same reason.

    A good indicator of successful first-boot setup is the printer's LEDs turning off
    and then back on as the MCUs initialise under Klipper control.

## Switching back to standalone K1 / Simple AF

1. #### Uninstall pik1 from K1
    ```sh
    make uninstall-k1
    ```
    This removes the init script and binaries and restores all disabled services.
    Alternatively:
    ```sh
    mv /etc/init.d/S99pik1 /etc/init.d/_S99pik1
    mv /etc/init.d/_S55klipper_service  /etc/init.d/S55klipper_service
    mv /etc/init.d/_S56moonraker_service /etc/init.d/S56moonraker_service
    # restore any other services as needed
    ```

2. #### Stop Pi service
    ```bash
    sudo systemctl stop pik1
    ```
    Optional -- the service will just sit idle if left running.

3. #### Revert printer.cfg and recable
    Revert `printer.cfg` serial paths and reconfigure cables as needed.

    You can run both the Pi and K1 standalone simultaneously (e.g. for camera
    services) without conflict as long as the pik1 init script is disabled.

## Optional extras

- **Pi mount:** Once everything is working you can print
  [a nice combined mount](https://www.printables.com/model/585116-motherboard-cover-optional-rpi-mount-extension-cre)
  for the K1 mainboard and a Raspberry Pi.
- **KlipperScreen:** The Pi runs KlipperScreen by default. Connect an HDMI
  touchscreen to the Pi's HDMI port to use it, or uninstall it if not needed.
