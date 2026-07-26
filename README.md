# ISO2Drive

Flash a Linux ISO to a drive and boot it anywhere — a **frugal-install** tool with
two frontends over one shared C core:

- **AppImage (Linux host)** — *greenfield*: owns a target disk, installs GRUB for
  UEFI + BIOS with two `grub-install` calls.
- **Windows installer** — *brownfield*: adds Linux-ISO booting alongside an existing
  Windows install (Grub2Win-style), no repartition, Windows boot files untouched.

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
  backend_linux.c    AppImage host — grub-install x2          [REAL]
  backend_windows.c  Windows host — ESP + bcdedit orchestration [REAL, needs assets]
  winutil.c          Windows probes (firmware, Secure Boot, BitLocker, ...)
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
iso2drive doctor                            # report firmware / boot environment
iso2drive add /mnt/data/iso2drive ubuntu-24.04.iso   # stage an ISO (no boot code)
```

**Everything that rewrites boot config is a dry-run until you pass `--commit`.**

### Flash boot code — Linux (greenfield)

Run as root with the target ESP mounted:

```bash
iso2drive add /mnt/data/iso2drive ubuntu-24.04.iso --esp /mnt/esp --disk /dev/sdb --commit
```

`--esp` runs `grub-install` for UEFI (`--removable`, no NVRAM edits); adding `--disk`
also installs BIOS boot code (`i386-pc`). The master `grub.cfg` is written to
`<esp>/grub/grub.cfg`. Drop `--commit` to preview the exact commands.

### Flash boot code — Windows (brownfield dual-boot)

Preview from any prompt, then apply from an **elevated** prompt:

```bash
iso2drive add C:\iso2drive ubuntu-24.04.iso --flash
iso2drive add C:\iso2drive ubuntu-24.04.iso --flash --commit
```

The Windows backend matches whatever firmware mode Windows already uses:

- **UEFI** — mounts the existing ESP, copies `grubx64.efi` into `\EFI\ISO2Drive\`,
  and adds a firmware boot entry with `bcdedit` (Windows Boot Manager stays intact;
  GRUB's menu chainloads Windows).
- **BIOS** — leaves the MBR alone and adds a `bcdedit` *bootsector* entry that
  chainloads a grub2 loader dropped on `C:` (EasyBCD-style).

`doctor` (and the start of every flash) reports **Secure Boot / Fast Startup /
BitLocker**, which are the usual reasons a Windows-side Linux dual-boot fails.

#### Required GRUB binaries (`assets/grub/`)

Windows has no `grub-install`, so the actual GRUB binaries must be bundled. Point
at them with `--assets <dir>` (default `assets/grub`):

```
assets/grub/
  x86_64-efi/grubx64.efi     # UEFI
  i386-pc/g2ldr              # BIOS loader   (from Grub2Win, or grub-mkimage)
  i386-pc/g2ldr.mbr          # BIOS boot sector
```

Build the standalone EFI once on any Linux box:

```bash
grub-mkstandalone -O x86_64-efi -o grubx64.efi \
  --modules="part_gpt part_msdos fat ntfs ext2 search search_fs_file configfile loopback chain normal" \
  "boot/grub/grub.cfg=./efi-prelude.cfg"
```

where `efi-prelude.cfg` finds the ESP copy of our menu and loads it:

```
search --no-floppy --file --set=root /EFI/ISO2Drive/grub.cfg
configfile /EFI/ISO2Drive/grub.cfg
```

## What's real vs stubbed

**Real:** distro detection; the master `grub.cfg`; per-ISO entry generation; the
store/marker layout; ISO copy; **`install_grub` on both backends** — Linux
(`grub-install` x2) and Windows (ESP + `bcdedit` orchestration with a full dry-run
preview, gated on the bundled GRUB binaries above).

**Not yet implemented:** greenfield disk partitioning, persistence, and bootable-USB
mode (see roadmap).

## Per-distro boot logic

Each generated entry tries the ISO's own `/boot/grub/loopback.cfg` first (the
robust path most modern ISOs support), and only falls back to a hand-crafted
kernel/cmdline from `src/core/profile.c`. That table is a **starting point** —
validate it against real ISOs; **GLIM (GRUB2 Live ISO Multiboot)** is the
authoritative reference. Families marked "needs loopback.cfg" (Fedora, Arch,
openSUSE) only boot ISOs that ship one.

## Roadmap

1. **Disk partitioning (greenfield)** — GPT + BIOS-boot + ESP + data partition,
   so the Linux backend can prepare a blank disk end to end.
2. **Bootable-USB mode** — write a *traditional* bootable USB from an ISO
   (hybrid-ISO / `dd`-style flash, plus a file-copy + syslinux path), the direct
   Balena-Etcher-style workflow, distinct from frugal install.
3. **Persistence** — casper-rw / Debian `persistence`, using leftover disk space.
4. **Windows polish** — bundle/auto-fetch the GRUB binaries; add a Secure Boot
   signed-shim path so `--commit` works without disabling Secure Boot.
5. Validate/extend the profile table against real images; multi-ISO menu polish.
