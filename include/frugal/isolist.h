#ifndef FRUGAL_ISOLIST_H
#define FRUGAL_ISOLIST_H

#include <stdbool.h>

/* A cached file listing of an ISO, produced by running a lister command
 * (bsdtar/tar). Lets the portable core ask "does this path exist in the ISO?"
 * without depending on any platform-specific ISO9660 reader. */
typedef struct isolist isolist_t;

isolist_t *isolist_run(const char *shell_cmd); /* run cmd, capture listing */
bool       isolist_has(const isolist_t *l, const char *path); /* path has leading '/' */
void       isolist_free(isolist_t *l);

#endif /* FRUGAL_ISOLIST_H */
