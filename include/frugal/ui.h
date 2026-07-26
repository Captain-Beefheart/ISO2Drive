#ifndef ISO2DRIVE_UI_H
#define ISO2DRIVE_UI_H

/* A light, Balena-Etcher-flavored CLI presentation: a teal banner and the
 * signature three-step strip (IMAGE -> DRIVE -> FLASH). */

void ui_init(void);   /* enable ANSI colors / UTF-8 on Windows consoles */
void ui_banner(void); /* app banner + the three-step strip */

/* Print one step line. n is 1..3; active picks teal vs dim. */
void ui_step(int n, int active, const char *label, const char *detail);

#endif /* ISO2DRIVE_UI_H */
