#include "frugal/profile.h"
#include <stddef.h>

/* Detection markers: any one identifies the family (Ubuntu/Mint/Pop!_OS share
 * casper; Kali/Tails share Debian live; Manjaro/EndeavourOS share archiso). */
static const char *det_casper[] = { "/casper/vmlinuz", "/casper/filesystem.squashfs", NULL };
static const char *det_debian[] = { "/live/filesystem.squashfs", "/live/vmlinuz", NULL };
static const char *det_fedora[] = { "/LiveOS/squashfs.img", "/images/install.img", NULL };
static const char *det_arch[]   = { "/arch/boot/x86_64/vmlinuz-linux", "/arch/version",
                                    "/manjaro/x86_64/rootfs.sfs", NULL };
static const char *det_suse[]   = { "/boot/x86_64/loader/linux", NULL };

/*
 * NOTE: this table is a STARTING POINT. Validate the paths/cmdlines/persistence
 * details against real ISOs -- GLIM (GRUB2 Live ISO Multiboot) is the
 * authoritative per-distro reference. Families with kernel == NULL only boot
 * ISOs that ship their own /boot/grub/loopback.cfg, and persist_param == NULL
 * marks families without frugal persistence support here.
 */
static const distro_profile_t g_profiles[] = {
    { DISTRO_CASPER, "Ubuntu / casper-based", "ubuntu", det_casper,
      "/casper/vmlinuz", "/casper/initrd",
      "boot=casper iso-scan/filename=%s quiet splash ---",
      "persistent", "casper-rw", false },

    { DISTRO_DEBIAN, "Debian live", "debian", det_debian,
      "/live/vmlinuz", "/live/initrd.img",
      "boot=live findiso=%s components quiet splash",
      "persistence", "persistence", true },

    { DISTRO_FEDORA, "Fedora / dracut live", "fedora", det_fedora,
      NULL, NULL, NULL,           /* needs embedded loopback.cfg */
      NULL, NULL, false },

    { DISTRO_ARCH,   "Arch (archiso)", "arch", det_arch,
      NULL, NULL, NULL,           /* img_dev/img_loop -> use loopback.cfg */
      NULL, NULL, false },

    { DISTRO_SUSE,   "openSUSE live", "suse", det_suse,
      NULL, NULL, NULL,
      NULL, NULL, false },
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
