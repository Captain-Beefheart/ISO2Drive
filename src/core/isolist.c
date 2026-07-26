#include "frugal/isolist.h"
#include "frugal/util.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

struct isolist { char *text; };

isolist_t *isolist_run(const char *cmd) {
    char *t = run_capture(cmd);
    if (!t) return NULL;
    isolist_t *l = malloc(sizeof *l);
    if (!l) { free(t); return NULL; }
    l->text = t;
    return l;
}

/* A tar/bsdtar listing has one path per line, no leading '/', dir entries end
 * in '/'. Match case-insensitively (ISO9660 without Rock Ridge upcases). */
bool isolist_has(const isolist_t *l, const char *path) {
    if (!l || !l->text) return false;
    const char *needle = (path[0] == '/') ? path + 1 : path;
    size_t nl = strlen(needle);
    for (const char *line = l->text; line && *line; ) {
        const char *eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        while (len > 0 && (line[len - 1] == '/' || line[len - 1] == '\r')) len--;
        if (len == nl) {
            size_t i = 0;
            for (; i < nl; ++i)
                if (tolower((unsigned char)line[i]) != tolower((unsigned char)needle[i])) break;
            if (i == nl) return true;
        }
        line = eol ? eol + 1 : NULL;
    }
    return false;
}

void isolist_free(isolist_t *l) {
    if (l) { free(l->text); free(l); }
}
