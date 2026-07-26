#ifndef FRUGAL_BACKEND_H
#define FRUGAL_BACKEND_H

#include <stdbool.h>

/* Where boot code should be installed (filled by the CLI, consumed by
 * install_grub). A greenfield backend partitions/mounts first, then hands one
 * of these to install_grub. */
typedef struct {
    const char *disk;       /* whole-disk device for BIOS core.img, e.g. /dev/sdb; NULL to skip */
    const char *esp_dir;    /* mounted EFI System Partition, e.g. /mnt/esp */
    const char *boot_dir;   /* GRUB boot dir; NULL -> use esp_dir */
    const char *assets_dir; /* bundled GRUB binaries (Windows); NULL -> "assets/grub" */
    bool do_uefi;
    bool do_bios;
    bool commit;            /* false = dry-run (print the plan, change nothing) */
} frugal_target_t;

/* A host backend supplies the platform-specific half of the tool: reading
 * ISOs, staging files on the target partition, and installing GRUB. The
 * portable core (detection, grub.cfg generation) never touches platform code. */
typedef struct frugal_backend {
    const char *name;

    /* --- ISO inspection --- */
    void *(*iso_open)(const char *iso_path);
    bool  (*iso_has_path)(void *handle, const char *path);
    void  (*iso_close)(void *handle);

    /* --- store management on the target partition --- */
    /* store_root is the tool's "iso2drive" dir, e.g. C:\iso2drive or
     * /mnt/data/iso2drive. prepare_store creates iso/ + entries.d/ and plants
     * the .iso2drive-store marker. */
    int (*prepare_store)(const char *store_root);
    int (*copy_iso)(const char *iso_path, const char *store_root, char **out_grub_path);

    /* --- greenfield disk provisioning (Linux only) --- */
    /* Partition + format + mount a BLANK disk (GPT: BIOS-boot + ESP + ext4 data).
     * Fills *out_esp_dir and *out_store_root (caller frees). Destructive; honors
     * commit (dry-run prints the plan). */
    int (*partition_disk)(const char *disk, bool commit,
                          char **out_esp_dir, char **out_store_root);

    /* --- boot install --- */
    int (*install_grub)(const frugal_target_t *tgt, const char *store_root);
    int (*probe_env)(void);
} frugal_backend_t;

const frugal_backend_t *backend_get(void);

#endif /* FRUGAL_BACKEND_H */
