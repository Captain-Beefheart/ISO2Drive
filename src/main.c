/* ISO2Drive CLI frontend. Thin: it wires the host backend to the portable
 * core (distro detection + grub.cfg generation) and presents an Etcher-style
 * three-step flow. */
#include "frugal/backend.h"
#include "frugal/profile.h"
#include "frugal/grubcfg.h"
#include "frugal/ui.h"
#include "frugal/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cmd_detect(const frugal_backend_t *b, const char *iso) {
    void *h = b->iso_open(iso);
    if (!h) { log_err("cannot inspect %s", iso); return 1; }
    const distro_profile_t *p = distro_detect(b->iso_has_path, h);
    b->iso_close(h);
    if (p) {
        log_info("detected: %s (class=%s, %s)", p->name, p->grub_class,
                 p->kernel ? "has hand-crafted fallback" : "needs embedded loopback.cfg");
        return 0;
    }
    log_warn("unknown family; will rely on the ISO's embedded loopback.cfg");
    return 0;
}

static int cmd_gencfg(const char *out) {
    if (grubcfg_write_master(out) != 0) { log_err("write failed"); return 1; }
    log_info("wrote master grub.cfg -> %s", out);
    return 0;
}

static int cmd_doctor(const frugal_backend_t *b) {
    ui_banner();
    log_info("backend: %s", b->name);
    b->probe_env();
    return 0;
}

static int cmd_format_disk(const frugal_backend_t *b, int argc, char **argv) {
    const char *disk = argv[2];
    bool commit = false;
    for (int i = 3; i < argc; ++i)
        if (!strcmp(argv[i], "--commit")) commit = true;
    ui_banner();
    char *esp = NULL, *store = NULL;
    int rc = b->partition_disk(disk, commit, &esp, &store);
    if (rc == 0 && commit) log_info("ESP mounted at %s ; store at %s", esp, store);
    else if (rc == 0)      log_info("dry-run — re-run as root with --commit to partition %s", disk);
    free(esp); free(store);
    return rc ? 1 : 0;
}

/* Full greenfield chain: partition a blank disk, stage the ISO, install GRUB. */
static int cmd_provision(const frugal_backend_t *b, int argc, char **argv) {
    const char *disk = argv[2];
    const char *iso  = argv[3];
    bool commit = false, do_uefi = true, do_bios = true;
    const char *persist_size = NULL;
    for (int i = 4; i < argc; ++i) {
        if      (!strcmp(argv[i], "--commit"))  commit = true;
        else if (!strcmp(argv[i], "--no-uefi")) do_uefi = false;
        else if (!strcmp(argv[i], "--no-bios")) do_bios = false;
        else if (!strcmp(argv[i], "--persist") && i + 1 < argc) persist_size = argv[++i];
        else log_warn("ignoring unknown option: %s", argv[i]);
    }

    ui_banner();

    /* (1) IMAGE */
    ui_step(1, 1, "IMAGE", iso);
    void *h = b->iso_open(iso);
    const distro_profile_t *p = h ? distro_detect(b->iso_has_path, h) : NULL;
    if (h) b->iso_close(h);
    log_info("  %s", p ? p->name : "unknown family (will rely on loopback.cfg)");

    /* (2) DRIVE -- partition the blank disk */
    ui_step(2, 1, "DRIVE", disk);
    char *esp = NULL, *store = NULL;
    if (b->partition_disk(disk, commit, &esp, &store) != 0) { free(esp); free(store); return 1; }

    if (!commit) {
        log_info("dry-run — with --commit this then stages %s and installs GRUB to the new partitions", iso);
        free(esp); free(store);
        return 0;
    }

    if (b->prepare_store(store) != 0) { log_err("prepare_store failed"); free(esp); free(store); return 1; }
    char *grub_path = NULL;
    if (b->copy_iso(iso, store, &grub_path) != 0 || !grub_path) {
        log_err("copy_iso failed"); free(esp); free(store); return 1;
    }
    bool do_persist = false;
    if (persist_size) {
        if (p && p->persist_param) {
            char *dm = path_parent(store);
            do_persist = dm && b->create_persistence(dm, p->persist_label, p->persist_conf, persist_size, true) == 0;
            free(dm);
        } else {
            log_warn("persistence not supported for this distro family; ignoring --persist");
        }
    }

    char *base    = path_basename_noext(iso);
    char *slug    = str_slugify(base);
    char *title   = xstrdup(base ? base : "linux");
    char *entries = str_format("%s/entries.d", store);
    if (slug && title && entries && grubcfg_write_entry(entries, slug, title, grub_path, p, do_persist) == 0)
        log_info("  entry: entries.d/%s.cfg%s", slug, do_persist ? " (persistent)" : "");

    /* (3) FLASH */
    ui_step(3, 1, "FLASH", "installing boot code");
    frugal_target_t tgt = {0};
    tgt.esp_dir = esp; tgt.disk = disk;
    tgt.do_uefi = do_uefi; tgt.do_bios = do_bios; tgt.commit = true;
    int rc = b->install_grub(&tgt, store);

    free(esp); free(store); free(grub_path); free(base); free(slug); free(title); free(entries);
    return rc ? 1 : 0;
}

static int cmd_list_disks(const frugal_backend_t *b) {
    ui_banner();
    return b->list_disks();
}

static int cmd_write_usb(const frugal_backend_t *b, int argc, char **argv) {
    const char *iso    = argv[2];
    const char *device = argv[3];
    bool commit = false, verify = false, force = false;
    for (int i = 4; i < argc; ++i) {
        if      (!strcmp(argv[i], "--commit")) commit = true;
        else if (!strcmp(argv[i], "--verify")) verify = true;
        else if (!strcmp(argv[i], "--force"))  force = true;
        else log_warn("ignoring unknown option: %s", argv[i]);
    }
    ui_banner();
    ui_step(1, 1, "IMAGE", iso);
    ui_step(2, 1, "DRIVE", device);
    ui_step(3, commit ? 1 : 0, "FLASH",
            commit ? "raw write (dd-style)" : "dry-run (pass --commit to flash)");
    return b->write_usb(iso, device, commit, verify, force) ? 1 : 0;
}

static int cmd_add(const frugal_backend_t *b, int argc, char **argv) {
    const char *store = argv[2];
    const char *iso   = argv[3];

    frugal_target_t tgt = {0};
    tgt.do_uefi = true;
    bool flash = false;
    const char *persist_size = NULL;
    for (int i = 4; i < argc; ++i) {
        if      (!strcmp(argv[i], "--esp")      && i + 1 < argc) { tgt.esp_dir    = argv[++i]; flash = true; }
        else if (!strcmp(argv[i], "--disk")     && i + 1 < argc) { tgt.disk       = argv[++i]; tgt.do_bios = true; }
        else if (!strcmp(argv[i], "--boot-dir") && i + 1 < argc) { tgt.boot_dir   = argv[++i]; }
        else if (!strcmp(argv[i], "--assets")   && i + 1 < argc) { tgt.assets_dir = argv[++i]; }
        else if (!strcmp(argv[i], "--persist")  && i + 1 < argc) { persist_size   = argv[++i]; }
        else if (!strcmp(argv[i], "--flash"))   { flash = true; }
        else if (!strcmp(argv[i], "--commit"))  { tgt.commit = true; }
        else if (!strcmp(argv[i], "--no-uefi")) { tgt.do_uefi = false; }
        else if (!strcmp(argv[i], "--no-bios")) { tgt.do_bios = false; }
        else { log_warn("ignoring unknown option: %s", argv[i]); }
    }

    ui_banner();

    /* (1) IMAGE -- inspect the ISO */
    ui_step(1, 1, "IMAGE", iso);
    void *h = b->iso_open(iso);
    const distro_profile_t *p = h ? distro_detect(b->iso_has_path, h) : NULL;
    if (h) b->iso_close(h);
    log_info("  %s", p ? p->name : "unknown family (will rely on loopback.cfg)");

    /* (2) DRIVE -- stage onto the target */
    ui_step(2, 1, "DRIVE", store);
    b->probe_env();
    if (b->prepare_store(store) != 0) { log_err("prepare_store failed"); return 1; }

    char *grub_path = NULL;
    if (b->copy_iso(iso, store, &grub_path) != 0 || !grub_path) {
        log_err("copy_iso failed");
        return 1;
    }

    bool do_persist = false;
    if (persist_size) {
        if (p && p->persist_param) {
            char *dm = path_parent(store);
            int prc = dm ? b->create_persistence(dm, p->persist_label, p->persist_conf, persist_size, tgt.commit) : -1;
            do_persist = (prc == 0);
            if (prc != 0) log_warn("persistence not created; entry will be non-persistent");
            free(dm);
        } else {
            log_warn("persistence not supported for this distro family; ignoring --persist");
        }
    }

    char *base    = path_basename_noext(iso);
    char *slug    = str_slugify(base);
    char *title   = xstrdup(base ? base : "linux");
    char *entries = str_format("%s/entries.d", store);

    int rc = 1;
    if (slug && title && entries) {
        rc = grubcfg_write_entry(entries, slug, title, grub_path, p, do_persist);
        if (rc == 0) log_info("  entry: entries.d/%s.cfg%s", slug, do_persist ? " (persistent)" : "");
    }

    /* keep a reference master in the store; install_grub places the real one */
    char *master = str_format("%s/grub.cfg", store);
    if (master) { grubcfg_write_master(master); free(master); }

    /* (3) FLASH -- install boot code */
    const char *detail = !flash ? "staged (pass --flash to install boot code)"
                       : tgt.commit ? "installing boot code" : "dry-run (pass --commit to apply)";
    ui_step(3, flash ? 1 : 0, "FLASH", detail);
    if (flash) b->install_grub(&tgt, store);

    free(grub_path); free(base); free(slug); free(title); free(entries);
    return rc == 0 ? 0 : 1;
}

static void usage(void) {
    ui_banner();
    printf(
        "  usage:\n"
        "    iso2drive detect <iso>                        inspect an ISO\n"
        "    iso2drive gen-cfg <out.cfg>                   write the master grub.cfg\n"
        "    iso2drive doctor                              report firmware / boot environment\n"
        "    iso2drive add <store-dir> <iso> [flash opts]  stage an ISO (+ optionally flash)\n"
        "    iso2drive format-disk <disk> [--commit]       GREENFIELD: wipe+partition a blank disk\n"
        "    iso2drive provision <disk> <iso> [--commit]   GREENFIELD: partition + stage + flash\n"
        "    iso2drive list-disks                          list candidate target drives\n"
        "    iso2drive write-usb <iso> <device> [opts]     BOOTABLE USB: raw dd-style flash\n"
        "    iso2drive help\n"
        "\n"
        "  greenfield + write-usb ERASE the target (root/admin, dry-run by default).\n"
        "  write-usb options:  --commit  --verify  --force (allow non-removable targets)\n"
        "\n"
        "  flash options for 'add':\n"
        "    --flash          also install boot code (Windows picks UEFI/BIOS automatically)\n"
        "    --commit         actually apply changes (default is a dry-run preview)\n"
        "    --esp <dir>      Linux: mounted EFI System Partition (implies --flash)\n"
        "    --disk <dev>     whole disk for BIOS boot code, e.g. /dev/sdb\n"
        "    --assets <dir>   Windows: bundled GRUB binaries (default: assets/grub)\n"
        "    --boot-dir <d>   GRUB boot dir (default: the ESP)\n"
        "    --persist <size> Linux: add a writable persistence store, e.g. 4G (Ubuntu/Debian)\n"
        "    --no-uefi / --no-bios\n"
        "\n"
        "  <store-dir> is the tool's 'iso2drive' dir on the target partition,\n"
        "  e.g. /mnt/data/iso2drive or C:\\iso2drive.\n");
}

int main(int argc, char **argv) {
    ui_init();
    const frugal_backend_t *b = backend_get();
    if (argc < 2) { usage(); return 1; }
    if (!strcmp(argv[1], "detect")  && argc == 3) return cmd_detect(b, argv[2]);
    if (!strcmp(argv[1], "gen-cfg") && argc == 3) return cmd_gencfg(argv[2]);
    if (!strcmp(argv[1], "doctor")  && argc == 2) return cmd_doctor(b);
    if (!strcmp(argv[1], "format-disk") && argc >= 3) return cmd_format_disk(b, argc, argv);
    if (!strcmp(argv[1], "provision")   && argc >= 4) return cmd_provision(b, argc, argv);
    if (!strcmp(argv[1], "list-disks")  && argc == 2) return cmd_list_disks(b);
    if (!strcmp(argv[1], "write-usb")   && argc >= 4) return cmd_write_usb(b, argc, argv);
    if (!strcmp(argv[1], "add")     && argc >= 4) return cmd_add(b, argc, argv);
    if (!strcmp(argv[1], "help")) { usage(); return 0; }
    usage();
    return 1;
}
