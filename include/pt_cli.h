#ifndef PT_CLI_H
#define PT_CLI_H
#include <stdio.h>
/* The window-free commands; argv[1] is the command. Returns the exit code. */
int pt_cli_main(int argc, char **argv);
int pt_cli_is_command(const char *name);
const char *pt_cli_guide(void);
void pt_cli_usage(FILE *out);
#endif
