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
  ui.c            Etcher-style banner + three-step strip + progress bar
  flash.c         raw ISO->device streaming + read-back verify
src/backend/
  backend_linux.c    AppImage host — partition + grub-install + raw flash [REAL]
  backend_windows.c  Windows host — ESP + bcdedit orchestration + raw flash [REAL]
  winutil.c          Windows probes (firmware, Secure Boot, BitLocker, ...)
  winusb.c           Windows raw PhysicalDrive access (lock/dismount/write)
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

### Greenfield — Linux (blank disk, end to end)

`provision` wipes a blank disk, lays down GPT (1 MiB BIOS-boot + 300 MiB FAT32 ESP +
ext4 data), mounts it, stages the ISO, and installs GRUB for **both** UEFI and BIOS —
one command. Run as root; **dry-run until `--commit`**:

```bash
iso2drive provision /dev/sdb ubuntu-24.04.iso --commit
```

Or just partition/format/mount a disk, then stage separately:

```bash
iso2drive format-disk /dev/sdb --commit
iso2drive add /run/iso2drive/data/iso2drive ubuntu-24.04.iso --esp /run/iso2drive/esp --disk /dev/sdb --commit
```

Safety: greenfield commands **refuse the running system disk** and any disk with
mounted partitions, print the target's current layout before touching it, and need
`sgdisk` (gptfdisk), `mkfs.fat` (dosfstools), `mkfs.ext4` (e2fsprogs), and
`partprobe` (parted). `add --esp` alone (no partitioning) still works if you've
prepared the partitions yourself.

### Persistence (Linux)

Add a writable overlay so a frugally-booted live session remembers changes across
reboots. Linux backend only (it needs `mkfs.ext4`), for Ubuntu/casper and Debian live:

```bash
iso2drive provision /dev/sdb ubuntu-24.04.iso --persist 4G --commit
iso2drive add /mnt/data/iso2drive debian-12.iso --persist 2G --commit
```

This creates an ext4 image labelled `casper-rw` (Ubuntu) or `persistence` (Debian —
with a `/ union` persistence.conf) at the root of the store partition, and generates
a direct-boot **`(persistent)`** entry carrying the right kernel arg (`persistent` /
`persistence`). Other families, and the Windows backend, don't support persistence
(no ext4 tooling); `--persist` is ignored there with a warning.

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

## Bootable USB (raw / dd-style flash)

A separate mode from frugal install: write an ISO **byte-for-byte** onto a whole
device, the way Balena Etcher does. Modern Linux ISOs are isohybrid, so the result
boots directly on both BIOS and UEFI.

```bash
iso2drive list-disks                                       # find the target safely
iso2drive write-usb ubuntu-24.04.iso /dev/sdb              # dry-run preview (Linux)
iso2drive write-usb ubuntu-24.04.iso 2 --commit --verify   # flash + verify (Windows: drive #)
```

- Device: Linux `/dev/sdX`; Windows a drive number, `PhysicalDriveN`, or `\\.\PhysicalDriveN`.
- **Dry-run until `--commit`**; `--commit` needs root (Linux) / an elevated prompt (Windows).
- **Refuses the system disk** outright, and refuses non-removable drives unless `--force`.
- `--verify` reads the device back and compares it against the ISO.
- A live progress bar shows write (and verify) progress.

`write-usb` does the raw/`dd` path only — right for isohybrid Linux ISOs. Windows
ISOs and >4 GB files on FAT need the file-copy + syslinux path (roadmap).

## What's real vs stubbed

**Real:** distro detection; the master `grub.cfg`; per-ISO entry generation; the
store/marker layout; ISO copy; **greenfield disk partitioning** (Linux: `sgdisk` +
`mkfs` + mount, with a full dry-run preview and system-disk guards); **`install_grub`
on both backends** — Linux (`grub-install` x2) and Windows (ESP + `bcdedit`
orchestration, gated on the bundled GRUB binaries above); **bootable-USB raw flash**
on both backends (`list-disks` + `write-usb`, with progress bar, read-back verify,
and system-disk / non-removable guards); **persistence** (Linux: labelled ext4
writable image for Ubuntu/casper + Debian live, with the `(persistent)` entry).

**Not yet implemented:** the file-copy + syslinux USB path (see roadmap).

## Per-distro boot logic

Each generated entry tries the ISO's own `/boot/grub/loopback.cfg` first (the
robust path most modern ISOs support), and only falls back to a hand-crafted
kernel/cmdline from `src/core/profile.c`. That table is a **starting point** —
validate it against real ISOs; **GLIM (GRUB2 Live ISO Multiboot)** is the
authoritative reference. Families marked "needs loopback.cfg" (Fedora, Arch,
openSUSE) only boot ISOs that ship one.

## Roadmap

1. **File-copy USB path** — a FAT32/exFAT file-copy + syslinux/EFI variant of
   `write-usb`, for Windows ISOs and >4 GB payloads (raw `dd` covers isohybrid today).
2. **Windows polish** — bundle/auto-fetch the GRUB binaries; add a Secure Boot
   signed-shim path so `--commit` works without disabling Secure Boot.
3. **Persistence polish** — `writable`-label support for newest Ubuntu; dedicated
   persistence partition option; Windows persistence via an ext4 image library.
4. Validate/extend the profile table against real images; multi-ISO menu polish.
