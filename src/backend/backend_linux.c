/* Linux host backend (for the AppImage frontend).
 * Greenfield: owns a target disk, installs GRUB for UEFI + BIOS. */
#include "frugal/backend.h"
#include "frugal/isolist.h"
#include "frugal/grubcfg.h"
#include "frugal/util.h"

#include <stdlib.h>
#include <string.h>

/* Wrap a path in single quotes for /bin/sh, escaping embedded quotes. */
static char *sh_quote(const char *s) {
    size_t extra = 0;
    for (const char *p = s; *p; ++p) if (*p == '\'') extra += 3;
    char *out = malloc(strlen(s) + extra + 3);
    if (!out) return NULL;
    char *w = out;
    *w++ = '\'';
    for (const char *p = s; *p; ++p) {
        if (*p == '\'') { *w++ = '\''; *w++ = '\\'; *w++ = '\''; *w++ = '\''; }
        else *w++ = *p;
    }
    *w++ = '\'';
    *w = '\0';
    return out;
}

/* Run a command (commit) or just print it (dry-run). Returns 0 on success. */
static int run_step(bool commit, const char *cmd) {
    if (!commit) { log_info("(dry-run) %s", cmd); return 0; }
    log_info("+ %s", cmd);
    int rc = system(cmd);
    if (rc != 0) log_err("command failed (status %d): %s", rc, cmd);
    return rc;
}

static void *lin_iso_open(const char *iso) {
    char *q = sh_quote(iso);
    if (!q) return NULL;
    /* bsdtar reads ISO9660; GNU tar does not. Ships in libarchive-tools. */
    char *cmd = str_format("bsdtar -tf %s 2>/dev/null", q);
    free(q);
    if (!cmd) return NULL;
    isolist_t *l = isolist_run(cmd);
    free(cmd);
    if (!l) log_warn("could not read ISO (install libarchive-tools for bsdtar)");
    return l;
}
static bool lin_iso_has_path(void *h, const char *p) { return isolist_has((isolist_t *)h, p); }
static void lin_iso_close(void *h) { isolist_free((isolist_t *)h); }

static int lin_prepare_store(const char *store_root) {
    ensure_dir(store_root);
    char *iso  = str_format("%s/iso", store_root);
    char *ent  = str_format("%s/entries.d", store_root);
    char *mark = str_format("%s/.iso2drive-store", store_root);
    if (iso)  ensure_dir(iso);
    if (ent)  ensure_dir(ent);
    if (mark) write_file(mark, "ISO2Drive store marker\n");
    free(iso); free(ent); free(mark);
    return 0;
}

static int lin_copy_iso(const char *iso, const char *store_root, char **out_grub_path) {
    char *base = path_basename(iso);
    char *dst  = base ? str_format("%s/iso/%s", store_root, base) : NULL;
    int rc = -1;
    if (dst) {
        log_info("copying %s -> %s", iso, dst);
        rc = copy_file(iso, dst);
    }
    if (rc == 0 && out_grub_path) *out_grub_path = str_format("/iso2drive/iso/%s", base);
    free(base); free(dst);
    return rc;
}

/* Real GRUB install: UEFI (removable, no NVRAM) and/or BIOS (core.img on disk),
 * then drop the master grub.cfg where grub-install created its /grub dir.
 * Honors t->commit (dry-run prints the commands without running them). */
static int lin_install_grub(const frugal_target_t *t, const char *store_root) {
    (void)store_root;
    if (!t || !t->esp_dir) {
        log_err("install_grub: an ESP directory is required (--esp <mounted ESP>)");
        return -1;
    }
    bool commit = t->commit;
    if (commit && system("command -v grub-install >/dev/null 2>&1") != 0) {
        log_err("grub-install not found; install grub2 + grub-pc-bin + grub-efi-amd64-bin");
        return -1;
    }

    const char *boot = t->boot_dir ? t->boot_dir : t->esp_dir;
    char *qesp  = sh_quote(t->esp_dir);
    char *qboot = sh_quote(boot);
    if (!qesp || !qboot) { free(qesp); free(qboot); return -1; }

    int fail = 0;

    if (t->do_uefi) {
        char *cmd = str_format(
            "grub-install --target=x86_64-efi --efi-directory=%s "
            "--boot-directory=%s --removable --recheck", qesp, qboot);
        if (cmd) { fail |= (run_step(commit, cmd) != 0); free(cmd); } else fail = 1;
    }

    if (t->do_bios) {
        if (!t->disk) {
            log_err("BIOS install needs a whole-disk device (--disk, e.g. /dev/sdb)");
            fail = 1;
        } else {
            char *qdisk = sh_quote(t->disk);
            char *cmd = qdisk ? str_format(
                "grub-install --target=i386-pc --boot-directory=%s --recheck %s",
                qboot, qdisk) : NULL;
            if (cmd) { fail |= (run_step(commit, cmd) != 0); free(cmd); } else fail = 1;
            free(qdisk);
        }
    }

    /* grub-install creates <boot>/grub/; put our menu there. */
    char *cfg = str_format("%s/grub/grub.cfg", boot);
    if (cfg) {
        if (!commit) {
            log_info("(dry-run) write master grub.cfg -> %s", cfg);
        } else if (grubcfg_write_master(cfg) == 0) {
            log_info("wrote %s", cfg);
        } else {
            log_err("could not write %s", cfg);
            fail = 1;
        }
        free(cfg);
    } else fail = 1;

    free(qesp); free(qboot);
    if (!fail && commit)
        log_info("boot code installed (uefi=%s bios=%s)",
                 t->do_uefi ? "yes" : "no", t->do_bios ? "yes" : "no");
    return fail ? -1 : 0;
}

static int lin_probe_env(void) {
    char *fw = run_capture("[ -d /sys/firmware/efi ] && echo UEFI || echo BIOS");
    if (fw) {
        log_info("firmware: %s", (strncmp(fw, "UEFI", 4) == 0) ? "UEFI" : "Legacy BIOS");
        free(fw);
    }
    return 0;
}

static const frugal_backend_t g_backend = {
    "linux",
    lin_iso_open, lin_iso_has_path, lin_iso_close,
    lin_prepare_store, lin_copy_iso,
    lin_install_grub, lin_probe_env,
};
const frugal_backend_t *backend_get(void) { return &g_backend; }
