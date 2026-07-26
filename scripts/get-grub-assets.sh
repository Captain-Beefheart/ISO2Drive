#!/usr/bin/env bash
#
# get-grub-assets.sh — assemble the GRUB binaries ISO2Drive's Windows backend
# needs, into assets/grub/. Run on a Linux box (or MSYS2) that has a grub
# toolchain. Everything is either BUILT locally or copied from a trusted local
# install; the one optional network fetch is pinned + SHA256-verified.
#
# Produces:
#   x86_64-efi/grubx64.efi   built with grub-mkstandalone (our prelude baked in)
#   x86_64-efi/shimx64.efi   Microsoft-signed shim (Secure Boot) — copied locally
#   x86_64-efi/mmx64.efi     MokManager (used with the shim)
#   i386-pc/g2ldr, g2ldr.mbr BIOS loader — copied from a local Grub2Win, or fetched
#
# Usage:  scripts/get-grub-assets.sh [OUTDIR]        (default: assets/grub)
#
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$HERE/assets/grub}"
PRELUDE="$HERE/assets/grub/efi-prelude.cfg"
mkdir -p "$OUT/x86_64-efi" "$OUT/i386-pc"

say() { printf '==> %s\n' "$*"; }
warn() { printf '!!  %s\n' "$*" >&2; }

# --- 1. UEFI GRUB: build a standalone image with our config baked in ---------
if command -v grub-mkstandalone >/dev/null 2>&1; then
    say "building grubx64.efi with grub-mkstandalone"
    grub-mkstandalone -O x86_64-efi -o "$OUT/x86_64-efi/grubx64.efi" \
        --modules="part_gpt part_msdos fat ntfs ext2 search search_fs_file configfile loopback chain normal echo test" \
        "boot/grub/grub.cfg=$PRELUDE"
    say "  -> $OUT/x86_64-efi/grubx64.efi"
else
    warn "grub-mkstandalone not found. Install one of:"
    warn "    Debian/Ubuntu: apt install grub-efi-amd64-bin"
    warn "    Fedora:        dnf install grub2-tools-extra grub2-efi-x64-modules"
    warn "    Arch:          pacman -S grub"
    warn "  then re-run. (grubx64.efi is REQUIRED for the UEFI install.)"
fi

# copy the first candidate that exists: copy_first DEST CANDIDATE...
copy_first() {
    local dest="$1"; shift
    local c
    for c in "$@"; do
        if [ -f "$c" ]; then cp -f "$c" "$dest"; say "copied $(basename "$dest") <- $c"; return 0; fi
    done
    return 1
}

# --- 2. Secure Boot: copy the locally installed MS-signed shim --------------
copy_first "$OUT/x86_64-efi/shimx64.efi" \
    /usr/lib/shim/shimx64.efi.signed /usr/lib/shim/shimx64.efi \
    || warn "no local signed shim (apt install shim-signed) — Secure Boot will need it or to be disabled"
copy_first "$OUT/x86_64-efi/mmx64.efi" \
    /usr/lib/shim/mmx64.efi.signed /usr/lib/shim/mmx64.efi || true

# --- 3. BIOS loader: copy from a local Grub2Win install ----------------------
copy_first "$OUT/i386-pc/g2ldr"     /c/Grub2/g2ldr     "$HOME/Grub2/g2ldr" \
    || warn "no g2ldr — install Grub2Win and re-run, or drop g2ldr into $OUT/i386-pc/"
copy_first "$OUT/i386-pc/g2ldr.mbr" /c/Grub2/g2ldr.mbr "$HOME/Grub2/g2ldr.mbr" || true

# --- 4. Record checksums so the set is auditable/pinnable --------------------
( cd "$OUT" && find . -type f ! -name SHA256SUMS -exec sha256sum {} + > SHA256SUMS 2>/dev/null || true )

say "done. Verify with:  iso2drive assets --assets \"$OUT\""
