#include "frugal/ui.h"
#include <stdio.h>

#ifdef _WIN32
#  include <windows.h>
#  ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#    define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#  endif
#endif

/* Etcher-ish teal accent. */
#define TEAL "\x1b[38;2;42;209;209m"
#define DIM  "\x1b[2m"
#define BOLD "\x1b[1m"
#define RST  "\x1b[0m"

void ui_init(void) {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode))
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    SetConsoleOutputCP(CP_UTF8);
#endif
}

void ui_banner(void) {
    printf("\n  " BOLD TEAL "ISO2Drive" RST
           "  " DIM "— flash an ISO to a drive, boot it anywhere" RST "\n\n");
    printf("  " TEAL "①" RST " IMAGE   " DIM "›" RST "   "
              TEAL "②" RST " DRIVE   " DIM "›" RST "   "
              TEAL "③" RST " FLASH\n\n");
}

void ui_step(int n, int active, const char *label, const char *detail) {
    static const char *num[] = { "•", "①", "②", "③" };
    const char *color = active ? TEAL : DIM;
    const char *glyph = (n >= 1 && n <= 3) ? num[n] : num[0];
    printf("  %s%s %s%s  " DIM "▸" RST "  %s\n",
           color, glyph, label, RST, detail ? detail : "");
}
