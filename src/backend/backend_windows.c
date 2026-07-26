/* Windows host backend (for the Windows installer frontend).
 * Brownfield: adds Linux ISO booting next to an existing Windows install,
 * Grub2Win-style, without repartitioning and without overwriting Windows'
 * own boot files. Boot-config changes are dry-run by default (--commit to apply). */
#include "frugal/backend.h"
#include "frugal/isolist.h"
#include "frugal/util.h"
#include "winutil.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ---- ISO inspection ---- */

static void *win_iso_open(const char *iso) {
    char *q = str_format("\"%s\"", iso);
    if (!q) return NULL;
    /* Windows 10/11 ship bsdtar as tar.exe; libarchive reads ISO9660. */
    char *cmd = str_format("tar -tf %s 2>NUL", q);
    free(q);
    if (!cmd) return NULL;
    isolist_t *l = isolist_run(cmd);
    free(cmd);
    if (!l) log_warn("could not read ISO with tar.exe (requires Windows 10/11)");
    return l;
}
static bool win_iso_has_path(void *h, const char *p) { return isolist_has((isolist_t *)h, p); }
static void win_iso_close(void *h) { isolist_free((isolist_t *)h); }

/* ---- store management ---- */

static int win_prepare_store(const char *store_root) {
    ensure_dir(store_root);
    char *iso  = str_format("%s/iso", store_root);
    char *ent  = str_format("%s/entries.d", store_root);
    char *mark = str_format("%s/.iso2drive-store", store_root);
    if (iso)  ensure_dir(iso);
    if (ent)  ensure_dir(ent);
    if (mark) write_file(mark, "ISO2Drive store marker\r\n");
    free(iso); free(ent); free(mark);
    return 0;
}

static int win_copy_iso(const char *iso, const char *store_root, char **out_grub_path) {
    char *base = path_basename(iso);
    char *dst  = base ? str_format("%s/iso/%s", store_root, base) : NULL;
    int rc = -1;
    if (dst) {
        log_info("copying %s -> %s", iso, dst);
        rc = copy_file(iso, dst);
    }
    /* GRUB path is always POSIX-style relative to the store partition root. */
    if (rc == 0 && out_grub_path) *out_grub_path = str_format("/iso2drive/iso/%s", base);
    free(base); free(dst);
    return rc;
}

/* ---- boot install ---- */

/* Build a command, then run it (commit) or just print it (dry-run). */
static void stepf(bool commit, int *fail, const char *fmt, ...) {
    va_list ap;  va_start(ap, fmt);
    va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); *fail = 1; return; }
    char *cmd = malloc((size_t)n + 1);
    if (!cmd) { va_end(ap2); *fail = 1; return; }
    vsnprintf(cmd, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    if (!commit) {
        log_info("(dry-run) %s", cmd);
    } else {
        log_info("+ %s", cmd);
        int rc = system(cmd);
        if (rc != 0) { log_err("failed (%d): %s", rc, cmd); *fail = 1; }
    }
    free(cmd);
}

/* Create a BCD entry and return its {GUID}. In dry-run, returns a placeholder. */
static char *bcd_guid(bool commit, const char *create_cmd) {
    if (!commit) { log_info("(dry-run) %s", create_cmd); return xstrdup("{NEW-GUID}"); }
    log_info("+ %s", create_cmd);
    char *out = run_capture(create_cmd);
    if (!out) { log_err("bcdedit produced no output"); return NULL; }
    char *lb = strchr(out, '{');
    char *rb = lb ? strchr(lb, '}') : NULL;
    char *g = NULL;
    if (lb && rb && rb > lb) {
        size_t n = (size_t)(rb - lb + 1);
        g = malloc(n + 1);
        if (g) { memcpy(g, lb, n); g[n] = '\0'; }
    }
    if (!g) log_err("could not parse a GUID from: %s", out);
    free(out);
    return g;
}

static char *to_backslash(const char *s) {
    char *o = xstrdup(s);
    if (o) for (char *p = o; *p; ++p) if (*p == '/') *p = '\\';
    return o;
}

/* UEFI: drop a standalone grubx64.efi on the existing ESP and add a firmware
 * boot entry that points at it. Windows' own bootmgfw.efi is never touched. */
static int win_uefi_install(const char *assets, const char *store_root, bool commit) {
    char *efi_src = str_format("%s/x86_64-efi/grubx64.efi", assets);
    if (!efi_src) return -1;
    if (!file_exists(efi_src)) {
        log_warn("GRUB EFI not found at %s", efi_src);
        log_warn("build it once with grub-mkstandalone (see README), then drop it there.");
        if (commit) { free(efi_src); return -1; }
    }
    char *efi_bs   = to_backslash(efi_src);
    char *store_bs = to_backslash(store_root);
    char dl = win_free_drive_letter();
    if (!dl) dl = 'S';

    int fail = 0;
    log_info("UEFI install via the existing ESP, mounted at %c:", dl);
    stepf(commit, &fail, "mountvol %c: /s", dl);
    stepf(commit, &fail, "if not exist %c:\\EFI\\ISO2Drive mkdir %c:\\EFI\\ISO2Drive", dl, dl);
    stepf(commit, &fail, "copy /Y \"%s\" %c:\\EFI\\ISO2Drive\\grubx64.efi", efi_bs, dl);
    stepf(commit, &fail, "copy /Y \"%s\\grub.cfg\" %c:\\EFI\\ISO2Drive\\grub.cfg", store_bs, dl);

    char *guid = bcd_guid(commit, "bcdedit /copy {bootmgr} /d \"ISO2Drive (GRUB)\"");
    if (!guid) fail = 1;
    else {
        stepf(commit, &fail, "bcdedit /set %s path \\EFI\\ISO2Drive\\grubx64.efi", guid);
        stepf(commit, &fail, "bcdedit /set {fwbootmgr} displayorder %s /addfirst", guid);
        free(guid);
    }
    stepf(commit, &fail, "mountvol %c: /d", dl);

    free(efi_src); free(efi_bs); free(store_bs);
    if (!fail && commit) log_info("UEFI GRUB entry installed (its menu chainloads Windows)");
    return fail ? -1 : 0;
}

/* BIOS: non-destructive. Keep the Windows MBR; add a BCD bootsector entry that
 * chainloads a grub2 boot-sector image sitting on C:. (EasyBCD-style.) */
static int win_bios_install(const char *assets, const char *store_root, bool commit) {
    char *ldr = str_format("%s/i386-pc/g2ldr", assets);
    char *mbr = str_format("%s/i386-pc/g2ldr.mbr", assets);
    if (!ldr || !mbr) { free(ldr); free(mbr); return -1; }
    if (!file_exists(ldr) || !file_exists(mbr)) {
        log_warn("BIOS loader assets missing: %s and/or %s", ldr, mbr);
        log_warn("obtain g2ldr + g2ldr.mbr from Grub2Win, or build with grub-mkimage (i386-pc).");
        if (commit) { free(ldr); free(mbr); return -1; }
    }
    char *ldr_bs = to_backslash(ldr);
    char *mbr_bs = to_backslash(mbr);
    char *store_bs = to_backslash(store_root);

    int fail = 0;
    log_info("BIOS install via a non-destructive BCD bootsector entry (Windows MBR untouched)");
    stepf(commit, &fail, "if not exist C:\\ISO2Drive mkdir C:\\ISO2Drive");
    stepf(commit, &fail, "copy /Y \"%s\" C:\\ISO2Drive\\g2ldr", ldr_bs);
    stepf(commit, &fail, "copy /Y \"%s\" C:\\ISO2Drive\\g2ldr.mbr", mbr_bs);
    stepf(commit, &fail, "copy /Y \"%s\\grub.cfg\" C:\\ISO2Drive\\grub.cfg", store_bs);

    char *guid = bcd_guid(commit, "bcdedit /create /d \"ISO2Drive (GRUB)\" /application bootsector");
    if (!guid) fail = 1;
    else {
        stepf(commit, &fail, "bcdedit /set %s device partition=C:", guid);
        stepf(commit, &fail, "bcdedit /set %s path \\ISO2Drive\\g2ldr.mbr", guid);
        stepf(commit, &fail, "bcdedit /displayorder %s /addlast", guid);
        free(guid);
    }

    free(ldr); free(mbr); free(ldr_bs); free(mbr_bs); free(store_bs);
    if (!fail && commit) log_info("BIOS GRUB entry added to the Windows boot menu");
    return fail ? -1 : 0;
}

static int win_install_grub(const frugal_target_t *t, const char *store_root) {
    firmware_t fw = win_firmware_type();
    bool elevated = win_is_elevated();
    bool commit   = t && t->commit;
    const char *assets = (t && t->assets_dir) ? t->assets_dir : "assets/grub";

    log_info("firmware: %s | elevated: %s | mode: %s",
             win_firmware_name(fw), elevated ? "yes" : "no", commit ? "COMMIT" : "dry-run");

    /* Brownfield safety gate. */
    if (win_secure_boot_enabled() == 1)
        log_warn("Secure Boot is ON: GRUB needs a signed shim, or disable Secure Boot, or it won't load.");
    if (win_fast_startup_enabled() == 1)
        log_warn("Fast Startup is ON: turn it off before Linux writes to any NTFS volume.");
    if (win_bitlocker_on('C') == 1)
        log_warn("BitLocker is ON (C:): suspend it before changing boot config, or expect a recovery-key prompt.");

    if (commit && !elevated) {
        log_err("--commit rewrites boot config and requires an elevated (Run as administrator) prompt");
        return -1;
    }

    int rc;
    if (fw == FW_BIOS) {
        rc = win_bios_install(assets, store_root, commit);
    } else {
        if (fw == FW_UNKNOWN) log_warn("firmware type unknown; assuming UEFI");
        rc = win_uefi_install(assets, store_root, commit);
    }
    if (rc == 0 && !commit)
        log_info("dry-run complete — re-run elevated with --commit to apply");
    return rc;
}

static int win_partition_disk(const char *disk, bool commit,
                              char **out_esp, char **out_store) {
    (void)disk; (void)commit; (void)out_esp; (void)out_store;
    log_err("disk partitioning is greenfield-only (Linux backend / AppImage)");
    log_err("the Windows tool adds Linux booting to your EXISTING disk, no repartition");
    return -1;
}

static int win_probe_env(void) {
    firmware_t fw = win_firmware_type();
    int sb = win_secure_boot_enabled();
    int fs = win_fast_startup_enabled();
    int bl = win_bitlocker_on('C');
    log_info("firmware:      %s", win_firmware_name(fw));
    log_info("elevated:      %s", win_is_elevated() ? "yes" : "no");
    log_info("secure boot:   %s", sb == 1 ? "ON" : sb == 0 ? "off" : "unknown");
    log_info("fast startup:  %s", fs == 1 ? "ON" : fs == 0 ? "off" : "unknown");
    log_info("bitlocker C:   %s", bl == 1 ? "ON" : bl == 0 ? "off" : "unknown");
    return 0;
}

static const frugal_backend_t g_backend = {
    .name           = "windows",
    .iso_open       = win_iso_open,
    .iso_has_path   = win_iso_has_path,
    .iso_close      = win_iso_close,
    .prepare_store  = win_prepare_store,
    .copy_iso       = win_copy_iso,
    .partition_disk = win_partition_disk,
    .install_grub   = win_install_grub,
    .probe_env      = win_probe_env,
};
const frugal_backend_t *backend_get(void) { return &g_backend; }
