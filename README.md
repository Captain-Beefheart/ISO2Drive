# ISO2Drive

Flash a Linux ISO to a drive and boot it anywhere — a **frugal-install** tool with
two frontends over one shared C core:

- **AppImage (Linux host)** — *greenfield*: owns a target disk, installs GRUB for
  UEFI + BIOS with two `grub-install` calls.
- **Windows installer** — *brownfield*: adds Linux-ISO booting alongside an existing
  Windows install (Grub2Win-style), no repartition.

Both frontends emit the **same** `grub.cfg` and `entries.d/*.cfg`, so the fiddly
per-distro boot logic lives in exactly one place.

## How the boot menu recognizes things

Recognition is by *identity*, never by disk enumeration order (which changes the
moment a USB is plugged in). Everything is found with `search --file`:

| Thing | Signature |
|-------|-----------|
| This tool's ISO store | `search --file /iso2drive/.iso2drive-store` |
| Windows (UEFI) | `search --file /EFI/Microsoft/Boot/bootmgfw.efi` |
| Windows (BIOS) | `search --file /bootmgr` |

The master `grub.cfg` is static and never changes; each ISO you add drops one
`entries.d/<slug>.cfg` that the master sources in a loop.

## Layout

```
include/frugal/   public headers (core API + backend interface + ui)
src/core/         PORTABLE core — no platform code
  util.c          logging, strings, files, run_capture
  isolist.c       "does path X exist in the ISO?" over a tar listing
  profile.c       per-distro detection + boot profiles (edit me)
  grubcfg.c       master grub.cfg + per-ISO entry generation
  ui.c            Etcher-style banner + three-step strip
src/backend/
  backend_linux.c    AppImage host — grub-install x2         [install: REAL]
  backend_windows.c  Windows host — mountvol/bcdedit         [install: stubbed]
src/main.c        CLI frontend
```

## Build

MSYS2/MinGW (Windows) or any Linux:

```bash
make
```

Produces `iso2drive.exe` (Windows) or `iso2drive` (Linux). The Makefile auto-selects
the host backend from `$(OS)`.

## Usage

```bash
iso2drive gen-cfg out/grub.cfg              # emit the master grub.cfg
iso2drive detect ubuntu-24.04.iso           # distro detection
iso2drive add /mnt/data/iso2drive ubuntu-24.04.iso   # stage an ISO (no boot code)
```

Flash boot code too (Linux backend, run as root with the ESP mounted):

```bash
iso2drive add /mnt/data/iso2drive ubuntu-24.04.iso --esp /mnt/esp --disk /dev/sdb
```

`--esp` runs `grub-install` for UEFI (`--removable`, no NVRAM edits); adding `--disk`
also installs BIOS boot code (`i386-pc`). The master `grub.cfg` is written to
`<esp>/grub/grub.cfg`. `detect` reads the ISO with bsdtar — Windows 10/11 `tar.exe`
is bsdtar; on Linux install `libarchive-tools`.

## What's real vs stubbed

**Real:** distro detection, the master `grub.cfg`, per-ISO entry generation, the
store/marker layout, ISO copy, and **`install_grub` on the Linux backend**
(`grub-install` for UEFI + BIOS + placing the menu).

**Stubbed** (clearly marked, with the exact commands in comments): `install_grub`
on the Windows backend, disk partitioning, persistence, and the Windows brownfield
safety checks (Secure Boot, BitLocker, Fast Startup).

## Per-distro boot logic

Each generated entry tries the ISO's own `/boot/grub/loopback.cfg` first (the
robust path most modern ISOs support), and only falls back to a hand-crafted
kernel/cmdline from `src/core/profile.c`. That table is a **starting point** —
validate it against real ISOs; **GLIM (GRUB2 Live ISO Multiboot)** is the
authoritative reference. Families marked "needs loopback.cfg" (Fedora, Arch,
openSUSE) only boot ISOs that ship one.

## Roadmap

1. **Windows `install_grub`** — ESP mount + `bcdedit` UEFI entry + BIOS `core.img`
   (Grub2Win-style), preserving the existing Windows boot chain.
2. **Disk partitioning (greenfield)** — GPT + BIOS-boot + ESP + data partition,
   so the Linux backend can prepare a blank disk end to end.
3. **Bootable-USB mode** — write a *traditional* bootable USB from an ISO
   (hybrid-ISO / `dd`-style flash, plus a file-copy + syslinux path), the direct
   Balena-Etcher-style workflow, distinct from frugal install.
4. **Persistence** — casper-rw / Debian `persistence`, using leftover disk space.
5. **Windows brownfield safety** — detect + handle Secure Boot, BitLocker,
   Fast Startup.
6. Validate/extend the profile table against real images; multi-ISO menu polish.
