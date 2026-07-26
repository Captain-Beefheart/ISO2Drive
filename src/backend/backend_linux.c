/* Linux host backend (for the AppImage frontend).
 * Greenfield: owns a target disk, partitions it, and installs GRUB for UEFI + BIOS. */
#include "frugal/backend.h"
#include "frugal/isolist.h"
#include "frugal/grubcfg.h"
#include "frugal/flash.h"
#include "frugal/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

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

static bool have_cmd(const char *name) {
    char *c = str_format("command -v %s >/dev/null 2>&1", name);
    if (!c) return false;
    int rc = system(c);
    free(c);
    return rc == 0;
}

/* Partition N's device node: /dev/sdb -> /dev/sdb1, /dev/nvme0n1 -> /dev/nvme0n1p1. */
static char *part_dev(const char *disk, int n) {
    size_t len = strlen(disk);
    int needs_p = (len > 0 && disk[len - 1] >= '0' && disk[len - 1] <= '9');
    return str_format("%s%s%d", disk, needs_p ? "p" : "", n);
}

/* ---- ISO inspection ---- */

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

/* ---- store management ---- */

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

/* ---- greenfield disk provisioning ---- */

/* True if `disk` is the whole disk backing the running root filesystem. */
static bool disk_is_root_disk(const char *disk) {
    char *out = run_capture(
        "root=$(findmnt -no SOURCE / 2>/dev/null); "
        "lsblk -no PKNAME \"$root\" 2>/dev/null | head -n1");
    if (!out) return false;
    char *nl = strchr(out, '\n');
    if (nl) *nl = '\0';
    bool r = false;
    if (*out) {
        char *dev = str_format("/dev/%s", out);
        if (dev && strcmp(dev, disk) == 0) r = true;
        free(dev);
    }
    free(out);
    return r;
}

static bool disk_has_mounts(const char *qdisk) {
    char *cmd = str_format("lsblk -no MOUNTPOINT %s 2>/dev/null | grep -q .", qdisk);
    if (!cmd) return false;
    int rc = system(cmd);
    free(cmd);
    return rc == 0;
}

/* Partition (GPT: 1 MiB BIOS-boot + 300 MiB FAT32 ESP + ext4 data), format,
 * and mount a blank disk. Returns the ESP mount dir and the store root. */
static int lin_partition_disk(const char *disk, bool commit,
                              char **out_esp, char **out_store) {
    if (!disk) { log_err("partition_disk: no target disk"); return -1; }
    char *qdisk = sh_quote(disk);
    if (!qdisk) return -1;

    /* Show what is about to be destroyed (read-only). */
    char *lscmd = str_format("lsblk -o NAME,SIZE,TYPE,FSTYPE,LABEL,MOUNTPOINT %s 2>/dev/null", qdisk);
    if (lscmd) {
        char *o = run_capture(lscmd);
        free(lscmd);
        if (o && *o) log_info("target %s current layout:\n%s", disk, o);
        free(o);
    }

    /* Safety: never wipe the system disk or a disk with mounted partitions. */
    if (disk_is_root_disk(disk)) {
        log_err("refusing: %s holds the running root filesystem", disk);
        free(qdisk); return -1;
    }
    if (disk_has_mounts(qdisk)) {
        if (commit) {
            log_err("refusing: %s has mounted partitions; unmount them first", disk);
            free(qdisk); return -1;
        }
        log_warn("%s has mounted partitions; --commit will refuse until they are unmounted", disk);
    }

    if (commit) {
        const char *need[] = { "sgdisk", "mkfs.fat", "mkfs.ext4", "partprobe", NULL };
        for (int i = 0; need[i]; ++i)
            if (!have_cmd(need[i])) {
                log_err("missing tool: %s (need gptfdisk, dosfstools, e2fsprogs, parted)", need[i]);
                free(qdisk); return -1;
            }
    }

    char *p2 = part_dev(disk, 2), *p3 = part_dev(disk, 3);
    char *qp2 = sh_quote(p2), *qp3 = sh_quote(p3);
    const char *esp_mnt = "/run/iso2drive/esp";
    const char *data_mnt = "/run/iso2drive/data";
    int fail = 0;

    log_warn("GREENFIELD: this ERASES all data on %s", disk);

    struct { const char *fmt; char *arg; } steps[] = {
        { "wipefs -a %s", qdisk },
        { "sgdisk --zap-all %s", qdisk },
        { "sgdisk -n 1:0:+1MiB   -t 1:ef02 -c 1:\"BIOS boot\" %s", qdisk },
        { "sgdisk -n 2:0:+300MiB -t 2:ef00 -c 2:\"ESP\" %s",       qdisk },
        { "sgdisk -n 3:0:0       -t 3:8300 -c 3:\"ISO2Drive\" %s", qdisk },
        { "partprobe %s ; udevadm settle", qdisk },
        { "mkfs.fat -F32 -n ISO2DRV_ESP %s", qp2 },
        { "mkfs.ext4 -F -L ISO2DRIVE %s", qp3 },
    };
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; ++i) {
        char *c = str_format(steps[i].fmt, steps[i].arg);
        if (c) { fail |= (run_step(commit, c) != 0); free(c); } else fail = 1;
    }

    char *mk = str_format("mkdir -p %s %s", esp_mnt, data_mnt);
    if (mk) { fail |= (run_step(commit, mk) != 0); free(mk); }
    char *m1 = str_format("mount %s %s", qp2, esp_mnt);
    if (m1) { fail |= (run_step(commit, m1) != 0); free(m1); }
    char *m2 = str_format("mount %s %s", qp3, data_mnt);
    if (m2) { fail |= (run_step(commit, m2) != 0); free(m2); }

    if (out_esp)   *out_esp   = xstrdup(esp_mnt);
    if (out_store) *out_store = str_format("%s/iso2drive", data_mnt);

    free(qdisk); free(p2); free(p3); free(qp2); free(qp3);
    if (!fail && commit)
        log_info("partitioned + mounted: ESP=%s data=%s", esp_mnt, data_mnt);
    return fail ? -1 : 0;
}

/* ---- boot install ---- */

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

/* ---- bootable USB (raw flash) ---- */

typedef struct { int fd; } ldev;

static long ldev_write(void *c, const void *buf, size_t len) {
    int fd = ((ldev *)c)->fd;
    const char *p = buf;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, p + off, len - off);
        if (w <= 0) return -1;
        off += (size_t)w;
    }
    return (long)off;
}
static long ldev_read(void *c, void *buf, size_t len) {
    int fd = ((ldev *)c)->fd;
    char *p = buf;
    size_t off = 0;
    while (off < len) {
        ssize_t r = read(fd, p + off, len - off);
        if (r <= 0) return -1;
        off += (size_t)r;
    }
    return (long)off;
}
static int  ldev_rewind(void *c) { return lseek(((ldev *)c)->fd, 0, SEEK_SET) == (off_t)-1 ? -1 : 0; }
static void ldev_close(void *c) {
    ldev *d = c;
    if (d) {
        if (d->fd >= 0) {
#ifndef _WIN32
            fsync(d->fd); /* flush to the device before we drop the handle */
#endif
            close(d->fd);
        }
        free(d);
    }
}

/* /sys/block/<name>/removable: 1 = removable. */
static int disk_is_removable(const char *device) {
    const char *b = strrchr(device, '/');
    b = b ? b + 1 : device;
    char *cmd = str_format("cat /sys/block/%s/removable 2>/dev/null", b);
    if (!cmd) return -1;
    char *o = run_capture(cmd);
    free(cmd);
    int r = -1;
    if (o) { if (o[0] == '1') r = 1; else if (o[0] == '0') r = 0; free(o); }
    return r;
}

static int lin_write_usb(const char *iso, const char *device,
                         bool commit, bool verify, bool force) {
    if (disk_is_root_disk(device)) {
        log_err("refusing: %s is the running system disk", device);
        return -1;
    }
    int rem = disk_is_removable(device);
    log_info("target: %s  removable=%s", device, rem == 1 ? "yes" : rem == 0 ? "no" : "?");

    if (!commit) {
        log_info("(dry-run) would raw-write %s -> %s (dd-style)%s", iso, device, verify ? " + verify" : "");
        if (rem == 0) log_warn("%s is NOT removable; --commit will need --force", device);
        log_info("re-run as root with --commit to flash");
        return 0;
    }
    if (rem == 0 && !force) {
        log_err("%s is not removable; pass --force to override", device);
        return -1;
    }

    int fd = open(device, O_RDWR);
    if (fd < 0) { log_err("cannot open %s (need root?)", device); return -1; }
    ldev *d = malloc(sizeof *d);
    if (!d) { close(fd); return -1; }
    d->fd = fd;

    flash_dev_t dev = {
        .ctx = d, .align = 512,
        .write = ldev_write, .read = ldev_read, .rewind = ldev_rewind, .close = ldev_close,
    };
    log_warn("raw-writing to %s — all existing data is destroyed", device);
    int rc = flash_iso_to_dev(iso, &dev, verify);
    dev.close(dev.ctx);
    return rc;
}

static int lin_list_disks(void) {
    char *o = run_capture("lsblk -d -o NAME,SIZE,TYPE,TRAN,HOTPLUG,MODEL 2>/dev/null");
    if (o && *o) { fputs(o, stdout); free(o); }
    else { free(o); log_warn("lsblk not available"); }
    return 0;
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
    .name           = "linux",
    .iso_open       = lin_iso_open,
    .iso_has_path   = lin_iso_has_path,
    .iso_close      = lin_iso_close,
    .prepare_store  = lin_prepare_store,
    .copy_iso       = lin_copy_iso,
    .partition_disk = lin_partition_disk,
    .install_grub   = lin_install_grub,
    .probe_env      = lin_probe_env,
    .write_usb      = lin_write_usb,
    .list_disks     = lin_list_disks,
};
const frugal_backend_t *backend_get(void) { return &g_backend; }
