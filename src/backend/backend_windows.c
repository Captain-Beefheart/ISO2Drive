/* Windows host backend (for the Windows installer frontend).
 * Brownfield: adds Linux ISO booting next to an existing Windows install,
 * Grub2Win-style, without repartitioning. */
#include "frugal/backend.h"
#include "frugal/isolist.h"
#include "frugal/util.h"

#include <stdlib.h>
#include <string.h>

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

static int win_install_grub(const frugal_target_t *t, const char *store_root) {
    (void)t; (void)store_root;
    log_warn("install_grub (windows): TODO. Intended sequence (run elevated):");
    log_info("  mountvol S: /s                       :: expose the existing ESP");
    log_info("  copy grubx64.efi + modules -> S:\\EFI\\grub\\");
    log_info("  bcdedit /copy {bootmgr} /d \"GRUB2\"   :: add a UEFI boot entry");
    log_info("  bcdedit /set {guid} path \\EFI\\grub\\grubx64.efi");
    log_info("  (BIOS: write core.img to the disk MBR, Grub2Win-style)");
    log_info("  place the master grub.cfg at S:\\EFI\\grub\\grub.cfg");
    log_warn("check first: Secure Boot, BitLocker, Fast Startup (see README)");
    return 0;
}

static int win_probe_env(void) {
    char *o = run_capture("bcdedit /enum {current} 2>NUL");
    if (o && *o) {
        bool uefi = strstr(o, "winload.efi") != NULL;
        log_info("firmware (from BCD): %s", uefi ? "UEFI" : "Legacy BIOS");
    } else {
        log_warn("bcdedit unavailable; run elevated to detect firmware mode");
    }
    free(o);
    return 0;
}

static const frugal_backend_t g_backend = {
    "windows",
    win_iso_open, win_iso_has_path, win_iso_close,
    win_prepare_store, win_copy_iso,
    win_install_grub, win_probe_env,
};
const frugal_backend_t *backend_get(void) { return &g_backend; }
