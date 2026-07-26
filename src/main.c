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

static int cmd_add(const frugal_backend_t *b, int argc, char **argv) {
    const char *store = argv[2];
    const char *iso   = argv[3];

    frugal_target_t tgt = {0};
    tgt.do_uefi = true;
    bool flash = false;
    for (int i = 4; i < argc; ++i) {
        if      (!strcmp(argv[i], "--esp")      && i + 1 < argc) { tgt.esp_dir  = argv[++i]; flash = true; }
        else if (!strcmp(argv[i], "--disk")     && i + 1 < argc) { tgt.disk     = argv[++i]; tgt.do_bios = true; }
        else if (!strcmp(argv[i], "--boot-dir") && i + 1 < argc) { tgt.boot_dir = argv[++i]; }
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

    char *base    = path_basename_noext(iso);
    char *slug    = str_slugify(base);
    char *title   = xstrdup(base ? base : "linux");
    char *entries = str_format("%s/entries.d", store);

    int rc = 1;
    if (slug && title && entries) {
        rc = grubcfg_write_entry(entries, slug, title, grub_path, p);
        if (rc == 0) log_info("  entry: entries.d/%s.cfg", slug);
    }

    /* keep a reference master in the store; install_grub places the real one */
    char *master = str_format("%s/grub.cfg", store);
    if (master) { grubcfg_write_master(master); free(master); }

    /* (3) FLASH -- install boot code (only when a target ESP is given) */
    ui_step(3, flash ? 1 : 0, "FLASH",
            flash ? "installing boot code" : "staged (pass --esp to flash boot code)");
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
        "    iso2drive add <store-dir> <iso> [flash opts]  stage an ISO (+ optionally flash)\n"
        "    iso2drive help\n"
        "\n"
        "  flash options for 'add' (Linux backend):\n"
        "    --esp <dir>      mounted EFI System Partition (its presence enables flashing)\n"
        "    --disk <dev>     whole disk for BIOS boot code, e.g. /dev/sdb\n"
        "    --boot-dir <d>   GRUB boot dir (default: the ESP)\n"
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
    if (!strcmp(argv[1], "add")     && argc >= 4) return cmd_add(b, argc, argv);
    if (!strcmp(argv[1], "help")) { usage(); return 0; }
    usage();
    return 1;
}
