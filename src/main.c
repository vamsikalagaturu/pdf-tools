#include "pt_cli.h"
#include <string.h>

#ifdef PT_GUI
int pt_app_main(int argc, char **argv);
#endif

int main(int argc, char **argv)
{
#ifdef PT_GUI
    int command = argc > 1 && (!strcmp(argv[1], "help") || pt_cli_is_command(argv[1]));
    if (!command) return pt_app_main(argc, argv);
#endif
    return pt_cli_main(argc, argv);
}
