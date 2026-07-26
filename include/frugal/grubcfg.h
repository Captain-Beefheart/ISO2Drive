#ifndef FRUGAL_GRUBCFG_H
#define FRUGAL_GRUBCFG_H

#include "frugal/profile.h"

/* Write the static master grub.cfg: recognizes Windows (UEFI + BIOS) and
 * sources every file in entries.d. This file never changes after install. */
int grubcfg_write_master(const char *out_path);

/* Write entries.d/<slug>.cfg for one staged ISO.
 * iso_grub_path is the ISO location relative to the store partition as GRUB
 * sees it, e.g. "/iso2drive/iso/ubuntu-24.04.iso". prof may be NULL (generic). */
/* If persist is true and the family supports it, emits a direct-boot entry with
 * the persistence kernel arg (instead of the loopback.cfg-first form). */
int grubcfg_write_entry(const char *entries_dir,
                        const char *slug,
                        const char *title,
                        const char *iso_grub_path,
                        const distro_profile_t *prof,
                        bool persist);

#endif /* FRUGAL_GRUBCFG_H */
