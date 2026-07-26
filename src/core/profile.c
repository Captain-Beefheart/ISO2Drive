#include "frugal/profile.h"
#include <stddef.h>

/* Detection markers: a stable file that identifies each live-ISO family. */
static const char *det_casper[] = { "/casper/vmlinuz", NULL };
static const char *det_debian[] = { "/live/filesystem.squashfs", NULL };
static const char *det_fedora[] = { "/LiveOS/squashfs.img", NULL };
static const char *det_arch[]   = { "/arch/boot/x86_64/vmlinuz-linux", NULL };
static const char *det_suse[]   = { "/boot/x86_64/loader/linux", NULL };

/*
 * NOTE: this table is a STARTING POINT. Validate every path and cmdline against
 * real ISOs before trusting them -- GLIM (GRUB2 Live ISO Multiboot) is the
 * authoritative per-distro reference. Families with kernel == NULL are only
 * bootable via an embedded /boot/grub/loopback.cfg (their live boot needs
 * parameters too awkward to hand-craft, e.g. Arch's img_dev/img_loop pair).
 */
static const distro_profile_t g_profiles[] = {
    { DISTRO_CASPER, "Ubuntu / casper-based", "ubuntu", det_casper,
      "/casper/vmlinuz", "/casper/initrd",
      "boot=casper iso-scan/filename=%s quiet splash ---" },

    { DISTRO_DEBIAN, "Debian live", "debian", det_debian,
      "/live/vmlinuz", "/live/initrd.img",
      "boot=live findiso=%s components quiet splash" },

    { DISTRO_FEDORA, "Fedora / dracut live", "fedora", det_fedora,
      NULL, NULL, NULL }, /* needs embedded loopback.cfg */

    { DISTRO_ARCH,   "Arch (archiso)", "arch", det_arch,
      NULL, NULL, NULL }, /* img_dev/img_loop -> use loopback.cfg */

    { DISTRO_SUSE,   "openSUSE live", "suse", det_suse,
      NULL, NULL, NULL },
};
static const size_t g_nprofiles = sizeof g_profiles / sizeof g_profiles[0];

const distro_profile_t *distro_detect(iso_has_path_fn has, void *ctx) {
    for (size_t i = 0; i < g_nprofiles; ++i) {
        const distro_profile_t *p = &g_profiles[i];
        for (const char *const *m = p->detect; *m; ++m)
            if (has(ctx, *m)) return p;
    }
    return NULL;
}

const distro_profile_t *distro_by_family(distro_family_t f) {
    for (size_t i = 0; i < g_nprofiles; ++i)
        if (g_profiles[i].family == f) return &g_profiles[i];
    return NULL;
}
