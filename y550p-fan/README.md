# Y550P fan controller for Linux

`y550p-fand` is a small userspace daemon for the Lenovo IdeaPad Y550P. It
controls the internal fan through the command path already implemented by the
laptop's ENE embedded-controller firmware and verifies actual rotation using
the EC tachometer.

> [!WARNING]
> This program performs raw I/O to model-specific embedded-controller
> registers. It has been verified only on a Lenovo IdeaPad Y550P, machine type
> `20035`, Compal NIWBA LA-5371P motherboard, with BIOS `1ECN33WW(V5.07)`.
> Running similar code on a different machine can damage hardware. The daemon
> checks DMI data and refuses to start on unsupported systems.

## Why it exists

On the tested Y550P, the Linux `ideapad-laptop` `fan_mode` interface accepts a
value but the matching VPC command is a no-op in this EC firmware. The working
firmware path is:

```text
F502 command -> FB31 firmware cache -> FF11 / DAC1 -> EN_FAN1
```

The daemon writes only the firmware command byte at `F502`; it does not keep the
fan pin forced through a direct GPIO override. Rotation is measured through
`FE21` and the 12-bit tachometer value in `FE22:FE23`.

## Behaviour

- Reads the highest CPU, GPU, or ACPI temperature from Linux `hwmon` every
  500 ms.
- Applies a five-second `0xFF` startup kick, because the old fan can stick when
  starting at lower voltage.
- Uses this conservative curve:

  ```text
  below 55 C   0xD0
  55-64 C      0xE0
  65-74 C      0xF0
  75 C or more 0xFF
  ```

- Never intentionally stops the fan. Commands `0x70` and `0xA0` were unable to
  sustain this particular aged fan reliably.
- Repeats the full-speed kick after four invalid tachometer samples.
- Falls back to full speed when temperature sensors are unavailable.
- Preserves the original `F502` value across daemon crashes and restores it on
  a clean stop.
- Prevents two daemon instances from accessing the EC concurrently.

The estimated RPM assumes the EC's 64 microsecond monitor clock and two tach
pulses per revolution. It is intended for rotation/stall detection, not
laboratory-grade measurement.

## Requirements

- Lenovo IdeaPad Y550P / machine type `20035`
- x86-64 Linux with `hwmon` temperature sensors
- systemd
- GCC or Clang, GNU Make, and standard C library development headers
- root privileges for installation and `CAP_SYS_RAWIO` at runtime

On Ubuntu or Xubuntu, install the build tools with:

```bash
sudo apt install build-essential
```

## Build and install

Keep external cooling available during the first test and watch the physical
fan. Then run:

```bash
cd y550p-fan
make
sudo make install
```

The installation places:

```text
/usr/local/sbin/y550p-fand
/etc/systemd/system/y550p-fan.service
```

The service is enabled at boot and started immediately.

## Monitor

Current controller state:

```bash
cat /run/y550p-fan/status
```

Service state and live log:

```bash
systemctl status y550p-fan.service
journalctl -u y550p-fan.service -f
```

A healthy status contains `tach_valid=yes`, a non-zero `rpm`, and
`stock_path_ready=yes`.

## Uninstall

From this directory:

```bash
sudo make uninstall
```

Stopping the daemon restores the EC command saved when it first started.

## License

This component is distributed under the repository's MIT License.
