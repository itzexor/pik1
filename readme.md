# Connecting a Creality K1 to a Raspberry Pi

`pik1d` moves the K1's MCU serial ports to PTYs on a Raspberry Pi over one
USB connection. The Pi runs Klipper and Moonraker; the K1 remains the bridge
for its physical MCUs and, optionally, its touchscreen.

> This modifies the printer's normal software layout. Make sure you can
> restore the K1 before proceeding. Creality publishes
> [recovery images and instructions](https://github.com/CrealityOfficial/K1_Series_Annex/releases/tag/V1.0.0).

Daemon architecture, protocol flow, scheduling, and failure behavior are
documented in [DESIGN.md](DESIGN.md).

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

Prebuilt endpoint binaries are tracked as `build/k1/pik1d` and
`build/pi/pik1d`. To rebuild both:

```bash
make toolchain
make artifacts
make test
```

`make toolchain` downloads the musl cross-compilers into `.toolchain/`.
`make host` builds `build/host/pik1d` for the current machine; `make k1` and
`make pi` build one endpoint artifact. On an AArch64 host, `make pi` uses the
host compiler. `make test` requires a C++20 compiler for the pinned nanocobs
test suite.
Both endpoints must run the same protocol version, so deploy the K1 and Pi
binaries as a pair. Both install targets copy the utilities under `scripts/`
beside the daemon.

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
   pulls with `git pull --ff-only`, installs the tracked Pi binary and systemd
   units, and starts `pik1.service`.

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

### K1

1. Install [Simple AF](https://pellcorp.github.io/creality-wiki/) on the K1.

2. Clone this repository on the K1 and run:

   ```sh
   ./update.sh
   ```

   The updater installs `build/k1/pik1d` under `/usr/data/pik1`, installs
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
forwards that stream to Moonraker on the Pi.

Do not bind the K1 listener to `0.0.0.0` unless remote access is intentional.

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

A healthy installation brings the configured Klipper MCUs online. Use
`pik1d --control status` to check the peer release, protocol, and active
services.

Klipper may take about 15 seconds to reconnect while the GD32 bootloader is
active. `FIRMWARE_RESTART` can have the same delay.

## Peer commands

Use the daemon CLI to send commands to the other endpoint:

```sh
pik1d --control status
pik1d --control restart-pik1
pik1d --control reboot
pik1d --control poweroff
pik1d --control restart-wifi
pik1d --control restart-klipper
```

`status` returns one bounded summary containing the peer side, release,
protocol, and active logical services.
`restart-wifi` runs the installed `scripts/restart-wifi.sh` utility on the
peer without stopping the USB link. The utility resets the active Wi-Fi stack
and supports common embedded and Linux network managers.
`restart-klipper` restarts `klipper.service` when received by the Pi/PTY side.
The K1/MCU side acknowledges the command without taking any action.

### Shutdown propagation

Pi shutdown/reboot helper units send the matching command to the K1. A
K1-initiated action should use:

```sh
/usr/data/pik1/shutdown_command.sh poweroff
/usr/data/pik1/shutdown_command.sh reboot
```

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
