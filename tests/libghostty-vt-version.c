#include <ghostty/vt.h>

#include <stdio.h>
#include <string.h>

#if !GHOSTTY_VT_VERSION_AT_LEAST(1, 0, 0)
#error "libghostty-vt headers are older than 1.0.0"
#endif

int
main (void)
{
    const char *library = ghostty_version ();
    if (library == NULL || strncmp (library, "1.", 2) != 0) {
        fprintf (stderr, "headers %s, library %s\n", GHOSTTY_VT_VERSION,
                 library != NULL ? library : "(null)");
        return 1;
    }

    GhosttyTerminal terminal = NULL;
    if (ghostty_terminal_new (NULL, &terminal, 80, 24) != GHOSTTY_SUCCESS) {
        fprintf (stderr, "ghostty_terminal_new failed\n");
        return 1;
    }
    ghostty_terminal_free (terminal);
    printf ("headers %s, library %s\n", GHOSTTY_VT_VERSION, library);
    return 0;
}
