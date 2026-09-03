# duoctl

`duoctl` is an unofficial open-source Linux command-line utility for monitoring,
diagnosing, and eventually managing WD My Book Duo storage enclosures without
proprietary desktop software.

The current release is deliberately read-only. It reports WD-specific RAID
metadata, array health, rebuild progress, and the status of both drive bays.
Management commands are a future goal and are not present in this version.

## Supported hardware

Development and initial testing target the WD My Book Duo 25F6. Other My Book
Duo generations may use different commands or response layouts and should be
treated as unsupported until verified.

## Safety

The current implementation:

- opens the Linux SCSI generic device with `O_RDONLY`;
- sends only the fixed WD `GetRAIDStatus_Interlaken` data-in query (`A3/1F`);
- requests an 88-byte response using `SG_DXFER_FROM_DEV`; and
- contains no rebuild, configuration, erase, format, diagnostic-test, or other
  data-out command.

The source validates the returned WD signature and expected two-bay response
layout before decoding it. Even so, this is independently developed software:
back up important data and use it at your own risk.

## Build

On Debian or Ubuntu, install a compiler and Linux development headers:

```sh
sudo apt update
sudo apt install build-essential linux-libc-dev
cc -std=c11 -Wall -Wextra -O2 -o duoctl duoctl.c
```

No Python runtime or `sg3_utils` command is required by `duoctl` itself.

## Find the enclosure device

The program operates on the enclosure's SCSI generic character device, such as
`/dev/sg1`, not its mounted filesystem or `/dev/sdX` block device.

One convenient way to identify it is:

```sh
sudo apt install lsscsi
sudo lsscsi -g
```

Look for the WD enclosure/SES entry and note its `/dev/sgN` path. Device numbers
can change after rebooting or reconnecting hardware, so verify the path each
time rather than assuming it is always `/dev/sg1`.

## Run

```sh
sudo ./duoctl /dev/sg1
```

The display refreshes every five seconds. Press `R` to refresh immediately or
`Q` to quit. If no device is supplied, `/dev/sg1` is used.

## Example information

`duoctl` displays:

- RAID mode: RAID 0, RAID 1, or JBOD;
- array status: healthy, degraded, rebuilding, rebuild failed, or data loss;
- WD metadata status;
- rebuild percentage when reported by the enclosure; and
- left and right bay states, including online, obsolete/stale, missing, failed,
  rebuilding, spare, or rejected.

## Project direction

The goal is broader Linux support for WD My Book Duo enclosures. Additional
features will be added only after their protocol and safety characteristics are
understood. Read-only discovery and monitoring should remain usable separately
from any future state-changing commands.

Contributions containing original interoperability research are welcome. Do
not submit proprietary WD binaries, copied source code, or large decompiler
outputs.

## Licence

Copyright (C) 2026 duoctl contributors.

This project is free software licensed under the GNU General Public License,
version 3 or (at your option) any later version. See [`LICENSE`](LICENSE).

## Disclaimer

This project is independently developed, is not affiliated with or endorsed by
Western Digital, and uses WD and My Book Duo only to identify compatible
hardware. Western Digital, WD, and My Book are trademarks of their respective
owners.
