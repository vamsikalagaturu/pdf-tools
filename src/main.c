/* The one entry point of pdf-tools: commands go to cli.c, everything else to the window (app.c) when it is built
 * in. Without raylib (no PT_GUI) the binary has only the commands. */
#include "pt_cli.h"
#include "pt_core.h"
#include <string.h>

#ifdef PT_GUI
int pt_app_main(int argc, char **argv);
#endif

int main(int argc, char **argv)
{
#ifdef _WIN32
    pt_win_init();
#endif
#ifdef PT_GUI
    /* a first word that is neither an option nor an existing file is a mistyped command: it gets an error, not a
     * window that would block the caller */
    int command = argc > 1 && (!strcmp(argv[1], "help") || !strcmp(argv[1], "--version") ||
                               pt_cli_is_command(argv[1]) || (argv[1][0] != '-' && !pt_exists(argv[1])));
    if (!command) {
#ifdef _WIN32
        /* one console program for commands and the window: started from Explorer, the window keeps no console */
        pt_win_detach_console();
#endif
        return pt_app_main(argc, argv);
    }
#endif
    return pt_cli_main(argc, argv);
}
