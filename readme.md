# Connecting a Creality K1 to a Raspberry Pi

`pik1d` moves the K1's MCU serial ports to PTYs on a Raspberry Pi over one
USB connection. The Pi runs Klipper and Moonraker; the K1 remains the bridge
for its physical MCUs and, optionally, its touchscreen.

> This modifies the printer's normal software layout. Make sure you can
> restore the K1 before proceeding. Creality publishes
> [recovery images and instructions](https://github.com/CrealityOfficial/K1_Series_Annex/releases/tag/V1.0.0).

## Architecture

There is exactly one bidirectional USB bulk link:

```text
K1 USB host                         Raspberry Pi USB gadget
usbfs bulk URBs  <==== USB cable ====>  configfs + FunctionFS
        \                                  /
         +------ one sequenced link ------+
                  |      |       |
               control  MCU   TCP tunnel
```

The Pi creates one vendor-specific FunctionFS interface with one bulk OUT
endpoint and one bulk IN endpoint. The K1 finds that interface by VID:PID,
claims it through usbfs, and keeps bulk receive URBs posted. Both transports
feed and drain the same byte-oriented link; neither transport owns a separate
protocol session.

The link applies COBS framing, CRC32 integrity, session IDs, sequence numbers,
and bounded retransmission. After the channel-0 HELLO handshake succeeds, the
session router starts and routes:

| Wire channel | Logical service |
|---|---|
| 0 | Handshake, liveness, service state, and peer commands |
| 1–8 | MCU serial mux (`mcu:0`–`mcu:7` / `pty:0`–`pty:7`) |
| 15 | Optional touchscreen TCP tunnel |

Outbound traffic waits in bounded per-service queues. Control has highest
priority, MCU traffic is next, and tunnel traffic is last. Only a shallow
amount is admitted to the transport at once, so a touchscreen burst cannot
fill the USB path ahead of MCU traffic.

Any transport, framing, sequencing, or reliable-service failure tears down
the whole session. Queued bytes and logical endpoints are discarded, the
transport reconnects with bounded backoff, and services restart only after a
new HELLO handshake. See [FAILURE_MODEL.md](FAILURE_MODEL.md) for the failure
contract.

The TCP tunnel is for low-bandwidth Moonraker API traffic only. Do not use it
for webcam streams or file transfers.

## Requirements

- A K1 mainboard with the required internal USB connection.
- An SBC with a USB controller that supports peripheral/OTG mode. The supplied
  service targets a Raspberry Pi.
- A probe and K1/Pi software setup compatible with
  [Simple AF](https://pellcorp.github.io/creality-wiki/).
- A recovery path for the printer.

## Hardware

### Power

The Pi and K1 must share USB ground, but the USB VBUS line can backfeed power
between them. Block or disconnect VBUS on the printer-facing data cable while
leaving ground and shielding intact. Common options are a USB power blocker,
Kapton tape over the VBUS contact, or a purpose-built cable without the VBUS
wire.

Power the Pi independently through a USB-C power/data splitter, PoE, or a
properly regulated supply. Pi 5 power requirements are stricter than Pi 4
requirements.

### Data connections

- Connect the Pi's OTG-capable USB-C port to the K1 USB connection using a
  shielded data cable with VBUS blocked.
- Connect USB probes directly to the Pi.
- Move other USB devices, such as the camera, to the Pi if the K1 services
  that used them will be disabled.

## Build and install

Prebuilt K1 and Pi binaries are tracked as `build/pik1d.mipsel` and
`build/pik1d.aarch64`. To rebuild them:

```bash
make toolchain
make mipsel
make aarch64
make test
```

`make toolchain` downloads the musl cross-compilers into `.toolchain/`.
Both endpoints must run the same protocol version, so deploy the K1 and Pi
binaries as a pair.

### Raspberry Pi

1. Install [Simple AF for RPi](https://pellcorp.github.io/creality-wiki/rpi/).

2. Enable the Pi's USB controller in peripheral mode. On current Raspberry Pi
   OS images, add:

   ```text
   dtoverlay=dwc2
   ```

   to `/boot/firmware/config.txt`, add `modules-load=dwc2` to the single line
   in `/boot/firmware/cmdline.txt`, and add `libcomposite` to `/etc/modules`.
   Older images may use `/boot` instead of `/boot/firmware`. If enumeration
   still fails, use `dtoverlay=dwc2,dr_mode=peripheral`.

3. Reboot, clone this repository, and run:

   ```bash
   ./update.sh
   ```

   The updater auto-detects the Pi, stops and uninstalls an existing service,
   pulls with `git pull --ff-only`, installs the tracked AArch64 binary and
   systemd units, and starts `pik1.service`.

   To install without the updater:

   ```bash
   make install-pi
   ```

   `PI_DIR`, `PI_SYSTEMD_DIR`, and `SUDO` can override the defaults.

4. Point Klipper at the PTYs:

   ```ini
   [mcu]
   serial: /tmp/klipper_mcu
   restart_method: command

   [mcu nozzle_mcu]
   serial: /tmp/klipper_toolhead
   restart_method: command
   ```

   `restart_method: command` is required because the serial mux does not carry
   DTR or RTS.

The Pi service runs as root because `pik1d` owns configfs, the FunctionFS
mount and descriptors, UDC binding, and PTY permissions.

### K1

1. Install [Simple AF](https://pellcorp.github.io/creality-wiki/) on the K1.

2. Clone this repository on the K1 and run:

   ```sh
   ./update.sh
   ```

   The updater installs `build/pik1d.mipsel` under `/usr/data/pik1`, installs
   `/etc/init.d/S99pik1`, and disables these stock services while the K1 is in
   bridge mode:

   - `S50nginx_service`
   - `S50unslung`
   - `S50webcam`
   - `S55klipper_mcu`
   - `S55klipper_service`
   - `S56moonraker_service`

   Direct installation is also available:

   ```sh
   make install-k1
   ```

   Set `K1_DIR` consistently for install and uninstall if you do not want the
   default path.

3. Reboot both devices. The K1 daemon logs to `/tmp/pik1.log`; the Pi daemon
   logs to the systemd journal.

## Touchscreen tunnel

Screen support is enabled by default. The K1 listens only on
`127.0.0.1:7125`, where guppyscreen already expects Moonraker. The Pi side
forwards that logical stream to `127.0.0.1:7125` on the Pi:

```text
guppyscreen -> K1 localhost:7125 -> shared USB link -> Pi Moonraker:7125
```

The generated daemon arguments are equivalent to:

```sh
# K1
pik1d --usb \
    mcu:0:/dev/ttyS7:230400 \
    mcu:1:/dev/ttyS1:230400 \
    listen:127.0.0.1:7125

# Pi
pik1d --ffs \
    pty:0:/tmp/klipper_mcu \
    pty:1:/tmp/klipper_toolhead \
    forward:127.0.0.1:7125
```

Do not bind the K1 listener to `0.0.0.0` unless remote access is intentional.
The tunnel has no authentication of its own.

To omit the tunnel and disable the K1 screen services, use the same option on
both devices:

```sh
./update.sh --no-screen
```

The equivalent make option is `SCREEN=0`.

## Verification

On the Pi:

```bash
systemctl status pik1
journalctl -u pik1 -f
```

On the K1:

```sh
cat /tmp/pik1.log
```

A healthy startup shows:

- the Pi preparing and binding one FunctionFS gadget;
- the K1 opening the matching vendor bulk interface;
- a successful control HELLO on both sides;
- the serial service starting and the expected MCU/PTY channels becoming
  active;
- the tunnel listener starting when screen support is enabled.

Klipper may take about 15 seconds to reconnect while the GD32 bootloader is
active. `FIRMWARE_RESTART` can have the same delay.

## Peer commands

Each daemon exposes a root-only Unix socket at `/run/pik1/control.sock`.
Commands other than `status-peer` are acknowledged before the peer acts:

```sh
pik1d --control status-peer
pik1d --control restart-peer
pik1d --control reboot-peer
pik1d --control poweroff-peer
```

`status-peer` returns one bounded summary containing the peer side, release,
protocol, active logical services, and the peer's last received service state.
Only one outbound command may be pending at a time.

### Shutdown propagation

Pi shutdown/reboot helper units send the matching command to the K1. A
K1-initiated action should use:

```sh
/usr/data/pik1/shutdown_command.sh shutdown
/usr/data/pik1/shutdown_command.sh reboot
```

The receiving side writes `/run/pik1/peer-initiated` before acting. The Pi
helper units check that marker so they do not relay the action back to its
origin.

## Return the K1 to standalone mode

On the K1:

```sh
make uninstall-k1
reboot
```

This removes PiK1 and restores the services disabled by the install. On the
Pi:

```bash
make uninstall-pi
```

Restore the original Klipper serial configuration and USB cabling as needed.
