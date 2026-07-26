#ifndef FRUGAL_PROFILE_H
#define FRUGAL_PROFILE_H

#include <stdbool.h>

typedef enum {
    DISTRO_UNKNOWN = 0,
    DISTRO_CASPER,  /* Ubuntu / Mint / Pop!_OS / elementary */
    DISTRO_DEBIAN,  /* Debian live */
    DISTRO_FEDORA,  /* Fedora / RHEL live (dracut) */
    DISTRO_ARCH,    /* archiso */
    DISTRO_SUSE,    /* openSUSE live */
    DISTRO_COUNT
} distro_family_t;

typedef struct {
    distro_family_t   family;
    const char       *name;        /* human label */
    const char       *grub_class;  /* GRUB menuentry --class */
    const char *const *detect;     /* NULL-terminated marker paths inside the ISO */

    /* Fallback boot, used ONLY when the ISO ships no /boot/grub/loopback.cfg.
     * kernel/initrd are paths inside the ISO; cmdline_fmt takes one %s = the
     * ISO's GRUB path. If kernel is NULL, this family REQUIRES an embedded
     * loopback.cfg (too fiddly to hand-craft reliably). */
    const char *kernel;
    const char *initrd;
    const char *cmdline_fmt;
} distro_profile_t;

/* Callback the detector uses to probe the ISO. `path` has a leading '/'. */
typedef bool (*iso_has_path_fn)(void *ctx, const char *path);

const distro_profile_t *distro_detect(iso_has_path_fn has, void *ctx);
const distro_profile_t *distro_by_family(distro_family_t f);

#endif /* FRUGAL_PROFILE_H */
